# First boot of the SPNMX57 image

Flashed 2026-09-03 with the self-recovering test image
(`TEST-squashfs-sysupgrade.bin`, md5 `ae46a8598910048aaf127fb138f0f541`).

Full log: `collected/uart-spnmx57-first-boot.log`.

## It flashed, it booted, and it recovered itself exactly as designed

```
verifying sysupgrade tar file integrity
Writing from <stdin> to kernel ...  [ ][e][w][e][w]...
```

Then, across the captured UART session:

```
Machine model: Linksys SPNMX57      <- boot 1
Machine model: Linksys SPNMX57      <- boot 2
Machine model: Linksys SPNMX57      <- boot 3
Machine model: Linksys SPNMX56      <- boot 4, U-Boot flipped slots
```

**Three boots, then reverted.** That is precisely the behaviour the test image
was built for: `bootcount` deliberately has no `spnmx57` case, so `mtd resetbc`
never runs, the counter reaches the threshold, and U-Boot returns to the
known-good slot. The device is healthy and back on the SPNMX56 install with
nothing lost.

The DTS loaded correctly: `Hardware name: Linksys SPNMX57 (DT)`.

## Why Ethernet did not come up

A precise, named failure. The chain, in order:

```
[ 4.435] ssdk_dt_parse_mac_mode[300]:INFO:mac mode1 doesn't exit!
[ 4.435] ssdk_dt_parse_mac_mode[308]:INFO:mac mode2 doesn't exit!
[ 4.441] ssdk_dt_parse_default_mdio_bus[851]:ERROR:cannot find platform device from mdio node
[ 4.447] ssdk_dt_parse_mdio[888]:ERROR:mdio bus parse failed!
[ 5.991] regi_init[2578]:INFO:qca-ssdk module init, no device found!
[ 6.001] Unable to handle kernel access to user memory ... at virtual address 00000000000002e0
[ 6.111] lr : fal_mib_port_flush_counters+0x5c/0x224 [qca_ssdk]
          Call trace: __memset -> syn_dp_tx [qca_nss_dp] -> init_module [qca_nss_dp]
```

1. The SSDK cannot resolve the MDIO bus for the switch declared with
   `switch_access_mode = "mdio"`
2. So it registers **no** device at all: "no device found"
3. `qca_nss_dp` then initialises against that unregistered device and
   dereferences NULL, oopsing in `fal_mib_port_flush_counters`

The oops is a consequence, not the cause. Fix step 1 and the rest should follow.

## Leading hypothesis for the root cause

`ssdk_dts.c` looks for the MDIO bus by compatible string, and the strings it
knows are:

```c
mdio_node = of_find_compatible_node(NULL, NULL, "qcom,ipq40xx-mdio");
  ...      of_find_compatible_node(NULL, NULL, "qcom,qca-mdio");
  ...      of_find_compatible_node(NULL, NULL, "virtual,mdio-gpio");
```

But OpenWrt's `ipq5018.dtsi` declares the bus as:

```
compatible = "qcom,ipq5018-mdio";
```

**None of the three match.** The SPNMX56 never hits this because its only switch
uses `switch_access_mode = "local bus"`, so the MDIO lookup is never needed. Our
`ess-switch1@1` is the first node on this target to ask for
`switch_access_mode = "mdio"`, which is why this has not been seen before.

Note the vendor tree agrees: it declares its bus as
`compatible = "qcom,qca-mdio"` — one of the strings the SSDK looks for. OpenWrt
renamed it for the mainline driver, and the SSDK was never taught the new name.

## Two candidate fixes, cheapest first

1. **DTS only.** Append the string the SSDK looks for, keeping the mainline one
   first so the kernel driver still binds:

   ```
   &mdio1 {
           compatible = "qcom,ipq5018-mdio", "qcom,qca-mdio";
   };
   ```

   Needs checking that no other driver binds `qcom,qca-mdio`, and that
   `of_find_device_by_node()` then returns the platform device.

2. **Patch the SSDK** to also search `qcom,ipq5018-mdio`. More invasive, but the
   correct upstream fix if option 1 does not work, since the rename is
   OpenWrt's.

## What this settles, and what it does not

**Settled:** the image builds, flashes, boots, and the device tree is accepted.
The self-recovery mechanism works and is measured, not assumed. Wi-Fi came back
on every boot, so the caldata fix did its job.

**Not settled:** whether the QCA8386 forwards by default. We still have not got
far enough to find out, because the SSDK never bound to the switch.

---

# Second attempt: mdio0 fix confirmed working

Image md5 `7be5a97263124eeac9d35216f612b2d7`, with `&mdio0 { status = "okay"; }`
added.

**The MDIO error is gone.** Compare the two boots:

```
before:  ssdk_dt_parse_default_mdio_bus[851]:ERROR:cannot find platform device from mdio node
         ssdk_dt_parse_mdio[888]:ERROR:mdio bus parse failed!
after:   (neither line appears)
```

Device 0 now parses identically to the working SPNMX56, line for line. So the
diagnosis was right and the fix landed.

## Correction to the earlier hypothesis

The previous section blamed a compatible-string rename
(`qcom,ipq5018-mdio` vs `qcom,qca-mdio`). **That was wrong.** `mdio@88000`
actually carries both strings:

```
compatible = "qcom,ipq5018-mdio", "qcom,ipq40xx-mdio";
```

so the SSDK could always find the node. The real cause was that the SPNMX57 DTS
never enabled `&mdio0`, which was a deliberate but incorrect choice on the
grounds that MAC0 is not wired to a socket. The SSDK uses that bus as its
**default**, independently of whether anything is attached to it, and a disabled
node has no platform device for `of_find_device_by_node()` to return.

## Where it now stops

```
[ 6.110] ssdk_mp_reset_init[1311]:INFO:MP reset successfully!
[ 6.110] ssdk_dt_parse_mac_mode[308]:INFO:mac mode2 doesn't exit!
[ 6.115] ssdk_dt_parse_interrupt[942]:INFO:intr-gpio does not exist
[ 6.122] regi_init[2578]:INFO:qca-ssdk module init, no device found!
[ 6.131] Unable to handle kernel access ... at virtual address 0x2e0
[ 6.242] lr : fal_mib_port_flush_counters+0x5c/0x224 [qca_ssdk]
[ 6.432] Kernel panic - not syncing: Oops: Fatal exception
```

Note `ssdk_mp_reset_init: MP reset successfully!` — that line is **new**, and it
is device 1 (our QCA8386) being reset. So the second switch node is now being
processed, which it was not before.

## Reading the loop

From `ssdk_init.c` around line 2450, the per-device loop is:

```c
for (num = 0; num < dev_num; num++) {
        ...
        rv = ssdk_plat_init(&cfg, dev_id);
        SW_CNTU_ON_ERROR_AND_COND1_OR_GOTO_OUT(rv, -ENODEV);
        ssdk_driver_register(dev_id);
        rv = chip_ver_get(dev_id, &cfg);
        SW_CNTU_ON_ERROR_AND_COND1_OR_GOTO_OUT(rv, -ENODEV);
        ...
}
```

`CNTU` means **continue**: a device failing with `-ENODEV` is skipped and the
loop moves on. `rv` therefore holds the **last** device's result, so if device 0
registers and device 1 fails, the final message is still "no device found".

That fits: device 0 (`ess-switch@39c00000`, local bus) parses exactly as on the
working 56, while device 1 (`ess-switch1@1`, MDIO) fails at `chip_ver_get` —
it cannot read a chip version from the QCA8386.

## The next question, and it is a specific one

**How does the SSDK address the QCA8386 on the MDIO bus?**

Our `ess-switch1@1` node carries `device_id`, `switch_access_mode = "mdio"` and
`mdio-bus`, but **no MDIO address for the switch itself**. We know from the live
scan that the chip answers at address `0x11` via qca8k's page protocol, so the
hardware is reachable; the SSDK just may not know where to look.

Worth checking next, in order:

1. Whether `chip_ver_get` for `qcom,ess-switch-qca8386` uses a fixed MDIO
   address or expects a property we have not set
2. Whether the QCA8386 must be clocked by `qca_mht_hw_init` **before** it will
   answer a version read, which would be a chicken-and-egg the vendor solves
   somewhere in its init order
3. Raising `/sys/ssdk/log_level` to get more detail — though the panic happens
   before userspace, so this needs the panic prevented first

## Preventing the panic, to make debugging cheap

The oops is a **consequence**: `qca_nss_dp` initialises against the device the
SSDK never registered and dereferences NULL in
`fal_mib_port_flush_counters`. If `qca_nss_dp` is kept from auto-loading, the
box should boot and stay reachable over Wi-Fi, making everything above
inspectable live over SSH instead of one panic per build.

That is the cheapest next iteration.

## Self-recovery, twice now

Both flashes ended with the device healthy on its known-good install. Second
run: three SPNMX57 boots, three kernel panics, U-Boot flipped on the fourth
power-on, back on SPNMX56. **Measured, not assumed.**

Note the device's DHCP lease moved from `10.0.0.68` to `10.0.0.71` and back
during the cycling; find it by its `phy1-sta0` MAC `02:11:22:33:44:22` rather
than by a fixed address.
