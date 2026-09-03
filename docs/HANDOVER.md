# Handover: Linksys SPNMX57 OpenWrt Ethernet port

Repo: `~/Repositories/linksys-spnmx57-openwrt` -> `github.com/louij2/linksys-spnmx57-openwrt` (public)
Build host: `srv-openstack`, tree at `~/spnmx57/openwrt` (build in Docker image `owrt-deb12`)

## Where things stand

The device boots OpenWrt fine. Wi-Fi, LEDs, buttons, flash, serial, SSH, sysupgrade
with self-recovery all work. **The one remaining blocker is Ethernet.**

The switch is now fully reachable and the whole vendor bring-up sequence runs
correctly at the right point in probe. What has not happened is the four QCA8084
EPHYs appearing on the MDIO bus, so qca-ssdk's `chip_ver_get` still has nothing
to find.

Last image on the device: **build28**, the release build
(`70c8f52b95ba727e0b0f5a01274817a1`), published as
[v0.1.0-wifi-only](https://github.com/louij2/linksys-spnmx57-openwrt/releases/tag/v0.1.0-wifi-only).
It boots with zero warnings and one status line. The verbose bring-up
instrumentation still exists and is gated on the DTS property
`qcom,qca8084-preinit-debug`; add it next to `qcom,qca8084-preinit` to get back the
read-back verdict for every write, the strap addresses, the gate value, the MARKER
lines and the 32-address scan.

### Confirmed working (verified on hardware, not assumed)

- **The QCA8386 32-bit indirect register protocol.** Reads and writes both work,
  at the correct point in probe, before `of_mdiobus_register()`. See
  `docs/uboot-qca8084-protocol.md`.

  ```
  read32(addr):  write(phy 0x18, reg 0x0c, (addr & 0xffffff) >> 8)   // page select
                 udelay(100)
                 lo = read(phy 0x10, addr & 0x1c)
                 hi = read(phy 0x10, (addr & 0x1c) + 2)
                 return lo | (hi << 16)
  write32(addr, val): same page select, then write lo/hi to the same two regs
  ```

- **The whole vendor bring-up sequence**, both functions, in the vendor's order.
  Every register write lands and is observable:

  ```
  EPHY_CFG 0x00318820 before preinit -> 0x00020c41 after
  the two companion devices moved from MDIO addresses 4,5 to 5,6 when
    0x0c90f014 was written, which is direct proof the address writes take effect
  ```

- **LEDs**: all three (`red/green/blue:power`) register and are controllable.
- **Buttons**: `gpio_button_hotplug` loads, DTS nodes present, gpio27/28 correctly
  configured. Not physically press-tested; that driver fires uevents rather than
  registering an input device, so checking `/proc/bus/input/devices` is the wrong
  test and will look empty even when fine.

### THE BUG THAT WAS FIXED THIS SESSION

Every MDIO transaction returned `0xffffffff` because **the QCA8386 was held in
hardware reset for the entire duration of preinit**.

`__mdiobus_register()` is what claims the bus's reset GPIO and pulses it:
`devm_gpiod_get_optional(&bus->dev, "reset", GPIOD_OUT_HIGH)`, assert,
`reset-delay-us`, deassert, `reset-post-delay-us`. Moving preinit before
registration (which was necessary and correct) meant that had not run yet: tlmm
gpio24 was still unclaimed with a pull-down, sitting low, and the line is
`GPIO_ACTIVE_LOW`.

preinit now owns the line and does the pulse itself, and the DTS property is
deliberately renamed `reset-gpios` -> `qca8084-reset-gpios` so that
`__mdiobus_register()` does not re-request it (-EBUSY) or re-pulse it afterwards
and throw away everything preinit just did.

The driver logs the proof rather than assuming it:
`qca8084: reset gpio as found = 1 (1 = asserted, in reset)`.

### What Ghidra found after that (all now implemented)

1. **A missing register write**: `0x0c800304 &= ~0x1f`, between the per-EPHY reset
   pulses and the gate read. Added as `QCA8084_EPHY_HOLD`. Name is a guess, only
   its position in the sequence is confirmed.

2. **A missing whole function**: `FUN_4a94c7d0` assigns the MDIO addresses and had
   never been transcribed. It writes `0x20c41` over EPHY_CFG's low 20 bits (four
   5-bit fields = 1,2,3,4) and fills `0x0c90f014` with 5,6,7 via a first-free-bit
   scan over a constant in-use bitmap `0xffff001e`.

3. **The order**: `FUN_4a94b918` calls the address assignment at `0x4a94baa6` then
   the bring-up routine at `0x4a94baaa`. Addresses FIRST. Both orders were tried
   on hardware; neither made the EPHYs answer, but the vendor order is what is
   committed.

**The phy_address question is settled the other way round.** The strap really does
read 0,1,2,3, but that is only the power-on default -- the vendor drives the EPHYs
to 1,2,3,4. The DTS is back to the vendor's values, now with a reason. Corroborated
by the vendor's own in-use bitmap: bits 1-4 for the EPHYs, bits 16-31 for the switch
window, and 0x10-0x1f is exactly what answers on the live bus.

**The vendor calibration gate is honoured again**, not bypassed. It reads
`0x00080000`, on which the vendor's own condition `(((v & 0xffffff) >> 16) - 1) < 2`
is false, so the stock firmware -- which does bring Ethernet up on this board --
skips calibration on this silicon too. Running it anyway was the unvalidated
deviation.

## THE OPEN QUESTION

**Addresses 1, 2, 3 and 4 still report "MDIO device at address N is missing".**

The current live bus map, from the driver's own post-init scan and
`/sys/bus/mdio_bus/devices/`:

| addresses | what answers | reading |
|---|---|---|
| 1-4 | nothing | the four EPHYs, still silent |
| 5, 6 | PHY ID `0x00000000` | the two companion devices, moved there by our write |
| 0x10-0x1f | PHY ID `0x00020002` | the switch register window aliasing, not 16 PHYs |

The ID at 0x10-0x1f changes with page state (it read `0x00010001` on an earlier
boot), which is what confirms it is the indirect window and not real PHYs.

So: the switch is alive and configurable, the addresses are assigned, and the
EPHYs behind it are still not on the bus. Candidate next steps, roughly in order
of how cheap they are:

1. ~~Log whether the `0x0c800304` write actually landed.~~ **DONE, and it lands.**
   build27 added a read-back verdict to every write in the sequence. `0x0c800304`
   reads `0x0000001f` before (all five holds asserted) and `0x00000000` after. So do
   all the others: **27 writes, 27 landed.** That rules out the whole class of "a
   write is silently doing nothing", which was the cheapest remaining explanation.

   Two details worth keeping from that boot. Bit 31 of the per-EPHY clock registers
   sets itself after each reset assert and clears after each release, so the silicon
   is visibly acknowledging the writes rather than a bus returning stale values. And
   two of the eight isolate registers (`0x0c800118`, `0x0c800138`) sit at
   `0x80000000` rather than `0`.

2. **Give the EPHYs longer to come up.** The scan runs ~130ms after the last write.
   Try a settle of a second or two with a rescan loop before concluding they are
   absent. Cheap, and rules out a pure timing answer.

3. **Look for a work-mode / package-mode step earlier in the vendor flow.** The
   QCA8084 has switch-mode vs PHY-mode variants and it is plausible the EPHYs are
   only exposed on the SoC MDIO bus in one of them. `FUN_4a94b918` is the vendor
   init caller and is large and mostly U-Boot bookkeeping, but the region *before*
   `0x4a94baa6` has not been read yet. Start there.

4. Note that the earlier judgement that a list of functions "do not matter for
   detection" was made when the chip was in reset and nothing worked at all, so
   that conclusion rests on a false premise and is worth revisiting:
   `0x4a94d21c`, `0x4a94d694`, `0x4a94e004`, `0x4a94cb98`, `0x4a94cfb8`,
   `0x4a94cdb4`, `0x4a94cd04`, `0x4a94cd5c`, `0x4a94dca8`, `0x4a95a050`, `0x4a95a980`.

## THE BIG SHORTCUT: the binary carries function names

`strings` on `appsbl-mtd6.bin` returns actual vendor function names. This was found
late and changes how everything below should be approached -- stop hunting for
`FUN_4a94xxxx` by register constant and just locate the name string, find what
references it, and decompile the function around that reference.

```
ipq_qca8084_hw_init            (also "Error: ipq_qca8084_hw_init failed")
ipq_qca8084_work_mode_init
_qca8084_interface_mode_init
qca8084_switch_enable
ipq_qca8084_link_update
qca8084_port_speed_set / _duplex_set / _mac_speed_set / _rxmac_status_set / _txmac_status_set
/ess-switch/qca8084_swt_info          <- a device tree path
"GMAC%d:Get QCA8084_PHY "             <- the vendor successfully detecting a PHY
"uniphy callibration time out!"       <- a UNIPHY calibration step we do nothing like
"QCA8084-switch status:"
```

`qca8084_switch_enable`, `ipq_qca8084_work_mode_init` and the UNIPHY calibration are
the three names that most plausibly cover the gap between "chip configured" and
"PHYs answer MDIO". A partial run also placed `ipq_qca8084_work_mode_init` at
`FUN_4a94d21c`, called from `FUN_4a94b918` at `0x4a94bb86` -- the same parent as the
two functions already ported -- and suggested `FUN_4a94d694` is a reset-pulse-by-ID
helper driven by a clock/reset table. Both are leads to confirm, not established
facts. Note `FUN_4a94d21c` was on the "does not matter for detection" list.

## Ghidra

**Runs on `srv-openstack`, NOT on the Mac.** Five concurrent headless JVMs took the
MacBook's load average to 51 and made it unusable; the box is 16GB and Docker
Desktop is over-allocated on it already. srv-openstack has 80 cores and 246GB and
the whole workspace is mirrored there. Ghidra was verified there against a known
answer (it reproduces `DAT_4a94c7b4 = 0x0c800304`). Run it there and nowhere else.

- Analysed project: `~/ghidra-workspace/project` on srv-openstack
- Headless scripts: `~/ghidra-workspace/scripts/*.java` on srv-openstack
- A project can only be opened by one process at a time, so for parallel work copy
  it first: `cp -R ~/ghidra-workspace/project /tmp/gh-<key> && rm -f /tmp/gh-<key>/*.lock*`
  (20MB a copy; delete them afterwards, they add up fast)
- Run a headless script:
  ```bash
  ssh srv-openstack 'docker run --rm -v ~/ghidra-workspace/project:/project \
    -v ~/ghidra-workspace/input:/input:ro -v ~/ghidra-workspace/scripts:/scripts:ro \
    --entrypoint /ghidra/support/analyzeHeadless blacktop/ghidra:latest \
    /project appsbl -process appsbl-mtd6.bin -noanalysis \
    -scriptPath /scripts -postScript FindEphyEnable.java'
  ```
  Scripts must be `.java` -- `.py` needs PyGhidra which is not enabled in this image.
  **macOS has no `timeout`**, so do not wrap a local docker run in it.
  Script output arrives prefixed: strip with
  `sed 's/^INFO  <Script>.java> //; s/ (GhidraScript)  $//'`.

Scripts added this session: `FindEphyEnable.java` (scans all instructions and
defined data for a set of register constants and decompiles everything that
touches them), `DumpEphyLits.java` (resolves a literal pool to values),
`DumpAddrAssign.java`, `DumpInitSeq.java`.

Key addresses in `collected/uboot/appsbl-mtd6.bin`: `0x4a94c4dc` read32,
`0x4a94c524` write32, `0x4a94c35c` raw write, `0x4a94c3d0` raw read,
`0x4a94c630` the clock/reset/calibrate routine, `0x4a94c7d0` the MDIO address
assignment, `0x4a94b918` the init caller that calls both, `0x4a94c568` clk enable,
`0x4a94c57e` reset pulse.

Register map resolved from the literal pools:

| register | role |
|---|---|
| `0x0c8001a8` / `0x0c8001ac` | shared clock domains |
| `0x0c8001b0`..`0x0c8001bc` | per-EPHY clock domains |
| `0x0c800058`..`0x0c800158` step `0x20` | bit 0 cleared across eight registers |
| `0x0c800304` | bits [4:0] cleared ("EPHY_HOLD", meaning unconfirmed) |
| `0x0c900014` | calibration gate, reads `0x00080000` |
| `0x0c90f014` | companion MDIO addresses (written 5,6,7) |
| `0x0c90f018` | EPHY_CFG: four 5-bit PHY addresses + flags in [21:20] |
| `0x0c900048` / `0x60` / `0x68` / `0x5c` | per-port calibration sources |

## Build / flash / test loop

```bash
# full build
ssh srv-openstack 'cd ~/spnmx57/openwrt && rm -f bin/targets/qualcommax/ipq50xx/*.bin && \
  rm -rf build_dir/target-aarch64_cortex-a53_musl/linux-qualcommax_ipq50xx/linux-6.12.57/.prepared* \
         build_dir/target-aarch64_cortex-a53_musl/linux-qualcommax_ipq50xx/linux-6.12.57/drivers/net/mdio && \
  docker run --rm -u 1000 -v ~/spnmx57/openwrt:/build -w /build owrt-deb12 nice -n 15 make -j24 > /tmp/buildN.log 2>&1'
```

**Compile-check first.** `make target/linux/compile V=sc -j24` fails in about
25 minutes on a C error instead of wasting a full build, and `V=sc` is what
actually prints the compiler message -- a plain build only says
"target/linux failed to build".

**Patch generation.** Always diff against the true pristine source at
`/tmp/mdio-ipq4019-PRISTINE.c` on srv-openstack, NEVER against the `build_dir`
copy (which already has the patch applied). The working method:

```bash
# /tmp/work/a = pristine, /tmp/work/b = pristine + patch, edit b, then
diff -u a/drivers/net/mdio/mdio-ipq4019.c b/drivers/net/mdio/mdio-ipq4019.c
# then ALWAYS re-apply to a fresh pristine copy and diff the result against b
# to prove the patch round-trips before building
```

**Do not write C into a shell heredoc with escaped quotes.** It silently produced
`\"` in the source and cost a build cycle. Write the block to a local file, `scp`
it, and splice it in with a Python script that asserts the anchor is present and
unique. Same for the Python itself: an apostrophe in a comment closes a
single-quoted `ssh '...'` argument, so `scp` the script rather than inlining it.

**Verify the image is real before flashing.** Check the md5 changed, and grep
`vmlinux` for a string you just added -- `strings` on the sysupgrade image does
not find kernel strings (squashfs), but `vmlinux` works:

```bash
ssh srv-openstack 'strings ~/spnmx57/openwrt/build_dir/target-*/linux-*/linux-6.12.57/vmlinux | grep "your new log line"'
```

**Flash.** Device is found by MAC `02:11:22:33:44:22`, usually `10.0.0.70`; it is
on Wi-Fi (`phy1-sta0`), and the address can move on reboot.

```bash
scp -O build.bin root@10.0.0.70:/tmp/spnmx57.bin
ssh root@10.0.0.70 'sysupgrade -T -F /tmp/spnmx57.bin'          # dry run
ssh root@10.0.0.70 'setsid sysupgrade -F -v /tmp/spnmx57.bin'   # plain setsid, not setsid nohup
```

The `ubus call system sysupgrade ... (Connection failed)` line at the end is
normal -- it is the SSH session being torn down mid-upgrade, not a failure. It
alternates slots automatically and self-recovers, so a bad image is not fatal.
Device is back in about 45 seconds.

**Run copy, flash and poll as three separate commands.** Chaining them into one
long command hit a tool timeout mid-flash and silently left the old image
running, which then looked like the new code had failed. Always confirm the new
code is live by checking dmesg shows the log lines in the order the new source
has them, not just that the box rebooted.

`CONFIG_DEVMEM=n` in this kernel, and there is no `devmem` applet in busybox and
no mdio-tools, so there is no userspace path to the registers. Every experiment
has to go in the kernel patch and through a build cycle.

## Still open after Ethernet

- Remove the diagnostic MDIO scan and the 32 `ethernet-phy@N` children for a real image
- Re-enable `qca-nss-dp` once `chip_ver_get` succeeds
- Upstream the `qca808x.c` 6.12 signature fix as its own PR
- Parked: GL.iNet-style custom web UI
