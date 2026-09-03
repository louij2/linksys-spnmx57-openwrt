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
