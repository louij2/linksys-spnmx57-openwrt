# Handover: Linksys SPNMX57 OpenWrt Ethernet port

Repo: `~/Repositories/linksys-spnmx57-openwrt` -> `github.com/louij2/linksys-spnmx57-openwrt` (public)
Build host: `srv-openstack`, tree at `~/spnmx57/openwrt` (build in Docker image `owrt-deb12`)

## Where things stand

The device boots OpenWrt fine. Wi-Fi, LEDs, buttons, flash, serial, SSH, sysupgrade
with self-recovery all work. **The one remaining blocker is Ethernet**: the four
QCA8084 EPHYs behind the QCA8386 switch do not answer MDIO, so qca-ssdk's
`chip_ver_get` never finds the switch (`regi_init: no device found!`).

### Confirmed working (verified on hardware, not assumed)

- **The QCA8386 32-bit indirect register protocol**, reverse engineered from the
  vendor U-Boot via Ghidra, then proven bidirectionally on the live device:
  read EPHY_CFG -> `0x00318820` consistently across boots, and a write to clear
  bits [21:20] read back correctly as `0x00018820`. See
  `docs/uboot-qca8084-protocol.md`.

  ```
  read32(addr):  write(phy 0x18, reg 0x0c, (addr & 0xffffff) >> 8)   // page select
                 udelay(100)
                 lo = read(phy 0x10, addr & 0x1c)
                 hi = read(phy 0x10, (addr & 0x1c) + 2)
                 return lo | (hi << 16)
  write32(addr, val): same page select, then write lo/hi to the same two regs
  ```
  Every transaction is plain Clause 22. No new low-level MDIO code needed.

- **The EPHY strap addresses are 0,1,2,3** (decoded live from EPHY_CFG), NOT the
  1,2,3,4 the vendor's own DTS declares. `dts/ipq5018-spnmx57.dts` is fixed.

- **LEDs**: all three (`red/green/blue:power`) register and are controllable.
- **Buttons**: `gpio_button_hotplug` loads, DTS nodes present, gpio27/28 correctly
  configured (input, pull-up, idle high = active low). Not physically press-tested;
  that driver fires uevents rather than registering an input device, so checking
  `/proc/bus/input/devices` is the wrong test and will look empty even when fine.

### The current patch

`patches/0919-net-mdio-ipq4019-add-QCA8084-preinit-for-SPNMX57.patch`, applied to
`target/linux/qualcommax/patches-6.12/`. Gated on `qcom,qca8084-preinit` in the
`&mdio1` DTS node, so no other board is affected. It implements, transcribed from
Ghidra decompilation of `FUN_4a94c630`:

1. clock enable (set bit 0) + reset pulse (set bit 2, wait 21ms, clear) on
   `0x0c8001a8` and `0x0c8001ac`
2. clear bit 0 across eight registers, `0x0c800058` to `0x0c800158` step `0x20`
   (meaning unconfirmed, transcribed as-is)
3. same clock-enable + reset-pulse on the four per-EPHY domains
   `0x0c8001b0/b4/b8/bc`
4. per-port calibration: read a source register per port
   (`0x0c900048` / `0x60` / `0x68` / `0x5c`), extract two bitfields, write them
   into that EPHY's own MDIO debug registers `0x1d`/`0x1e`
5. clear EPHY_CFG bits [21:20]

**Currently the vendor's gate check is deliberately bypassed** (it reads
`0x00080000` on this hardware; the vendor condition wants field value 1 or 2, so
the vendor's own code would skip calibration here). Bypassing it was a
diagnostic, and it should probably be restored once the real blocker is found.

## THE IMMEDIATE NEXT STEP

**`build22` was running on `srv-openstack` when this session ended.** Check it:

```bash
ssh srv-openstack 'docker ps -q --filter ancestor=owrt-deb12 | grep -q . && echo building || md5sum ~/spnmx57/openwrt/bin/targets/qualcommax/ipq50xx/*sysupgrade.bin'
```

It contains a fix that has NOT yet been tested on hardware. Flash it and read
dmesg. **Success looks like `EPHY_CFG = 0x00318820` in the log (a real value)
rather than `0xffffffff`, and ideally MDIO addresses 0-3 no longer "missing".**

### Two bugs found and fixed this session, both mine, neither hardware

1. **Ordering**: `qca8084_preinit()` originally ran AFTER `of_mdiobus_register()`.
   That function is what scans the DTS `ethernet-phy@N` children and emits the
   "MDIO device at address N is missing" lines, so the scan was running before
   the chip was ever clocked. Moved before it. Verified with explicit
   `MARKER` log lines rather than inferred from timestamps.

2. **Uninitialised mutex** (this is what build22 fixes): `mutex_init(&bus->mdio_lock)`
   happens INSIDE `__mdiobus_register()` (line ~740 of `drivers/net/phy/mdio_bus.c`),
   NOT at allocation. So once preinit moved earlier, every `mdiobus_read`/`mdiobus_write`
   was locking an uninitialised mutex — undefined behaviour, and every transaction
   read back `0xffffffff`. Fixed by calling `bus->read`/`bus->write` **directly**;
   safe here because the bus is not yet visible to anything else so there is nothing
   to race with. `bus->reset(bus)` is also called explicitly at the top of preinit,
   since the controller's own clock/divider setup normally happens during registration.

**If build22 still shows `0xffffffff`**, the remaining suspect is that something
else in registration is needed before transactions work at this point in probe();
the fallback is to revert to running preinit AFTER `of_mdiobus_register()` (where
reads demonstrably worked and returned `0x00318820`) and instead remove the 32
diagnostic `ethernet-phy@N` children from the DTS entirely, doing the address scan
manually with `mdiobus_read` inside preinit after the bring-up sequence.

## Ghidra (set up this session, keep using it)

Everything is local on the Mac in Docker. **This is what resolved the protocol**;
hand-decoding Thumb-2 produced a confident, wrong answer twice before this.

- Analysed project: `~/ghidra-workspace/project` (binary already imported + analysed)
- Headless scripts: `~/ghidra-workspace/scripts/*.java`
- Browser GUI: `docker start ghidra-gui` then open **http://localhost:6080/vnc.html**
- Run a headless script:
  ```bash
  docker run --rm -v ~/ghidra-workspace/project:/project -v ~/ghidra-workspace/input:/input:ro \
    -v ~/ghidra-workspace/scripts:/scripts:ro --entrypoint /ghidra/support/analyzeHeadless \
    blacktop/ghidra:latest /project appsbl -process appsbl-mtd6.bin -noanalysis \
    -scriptPath /scripts -postScript DecompileTargets.java
  ```
  Scripts must be `.java` — `.py` needs PyGhidra which is not enabled in this image.

Key addresses in `collected/uboot/appsbl-mtd6.bin`: `0x4a94c4dc` read32,
`0x4a94c524` write32, `0x4a94c35c` raw write, `0x4a94c3d0` raw read,
`0x4a94c630` the whole clock/reset/calibrate routine, `0x4a94c568` clk enable,
`0x4a94c57e` reset pulse.

Functions already decompiled and found NOT to matter for detection (they are
U-Boot-internal bookkeeping or post-detection port/traffic config):
`0x4a94d21c`, `0x4a94d694`, `0x4a94e004`, `0x4a94cb98`, `0x4a94cfb8`,
`0x4a94cdb4`, `0x4a94cd04`, `0x4a94cd5c`, `0x4a94dca8`, `0x4a95a050`, `0x4a95a980`.

## Build / flash / test loop

```bash
# build
ssh srv-openstack 'cd ~/spnmx57/openwrt && rm -f bin/targets/qualcommax/ipq50xx/*.bin && \
  rm -rf build_dir/target-aarch64_cortex-a53_musl/linux-qualcommax_ipq50xx/linux-6.12.57/.prepared* \
         build_dir/target-aarch64_cortex-a53_musl/linux-qualcommax_ipq50xx/linux-6.12.57/drivers/net/mdio && \
  nohup docker run --rm -u 1000 -v ~/spnmx57/openwrt:/build -w /build owrt-deb12 nice -n 15 make -j24 > /tmp/buildN.log 2>&1 &'
```

**Patch generation gotcha that bit twice**: always diff against the true pristine
source at `/tmp/mdio-ipq4019-PRISTINE.c` on srv-openstack, NEVER against the
`build_dir` copy (which already has the patch applied — diffing against it produces
a patch whose context does not match pristine, and the build fails at `.prepared`).
Always `patch -p1 --dry-run` against a pristine copy before building.

Device: find by MAC `02:11:22:33:44:22`, usually `10.0.0.70`, address moves on reboot.
Flash: scp image to `/tmp/spnmx57.bin`, `sysupgrade -T -F` to dry-run, then
`setsid sysupgrade -F -v /tmp/spnmx57.bin` (plain `setsid`, not `setsid nohup`).
It alternates slots automatically and self-recovers, so a bad image is not fatal.

## Still open after Ethernet

- Restore the vendor gate check and remove the diagnostic MDIO scan for a real image
- Re-enable `qca-nss-dp` once `chip_ver_get` succeeds
- Upstream the `qca808x.c` 6.12 signature fix as its own PR
- Parked: GL.iNet-style custom web UI
