# Handover: Linksys SPNMX57 OpenWrt Ethernet port

Repo: `~/Repositories/linksys-spnmx57-openwrt` -> `github.com/louij2/linksys-spnmx57-openwrt` (public)
Build host: `srv-openstack` (aarch64, 80 cores, 246 GB), tree at `~/spnmx57/openwrt`,
built in Docker image `owrt-deb12`. **Ghidra runs there too, never on the Mac.**

## Where things stand

Everything on the device works except Ethernet. There is a published release for
using it as a wireless repeater in the meantime:
[v0.1.0-wifi-only](https://github.com/louij2/linksys-spnmx57-openwrt/releases/tag/v0.1.0-wifi-only).

The QCA8386 is fully reachable and configurable. Every register write in the
bring-up lands and reads back, bar one. **The four QCA8084 EPHYs still do not
answer Clause 22 MDIO at addresses 1-4**, so `qca_detect_phyid()` finds nothing
and qca-ssdk stops at `regi_init: qca-ssdk module init, no device found!`.

Device currently runs **build34**, a diagnostic build (verbose, all instrumentation
on). It works fine as a repeater. Reflash the release image if you want it quiet.

## THE MOST IMPORTANT THING TO KNOW

**Stop reverse engineering the bootloader. The vendor's Linux driver is in the
build tree and it is a complete reference implementation.**

```
~/spnmx57/openwrt/dl/qca-ssdk-2025.05.30~446db12b.tar.zst
  include/hsl/mht/mht_reg.h      <- the entire QCA8084 register map, named
  src/hsl/mht/mht_sec_ctrl.c     <- work mode, SKU, EPHY addresses
  src/init/ssdk_mht_clk.c        <- the whole clock/reset table and how to drive it
  src/init/ssdk_mht.c            <- qca_mht_hw_init, the full bring-up order
  src/init/ssdk_init.c           <- chip_ver_get / qca_detect_phyid
```

Extract it somewhere and read it before writing another line:

```bash
ssh srv-openstack 'mkdir -p /tmp/ssdk && cd /tmp/ssdk && \
  tar --zstd -xf ~/spnmx57/openwrt/dl/qca-ssdk-2025.05.30~446db12b.tar.zst'
```

Ghidra was necessary to get this far and is still useful for the bootloader-only
parts, but two of our register names turned out to be wrong guesses that this
source corrects immediately.

## Register map, named from the vendor driver

Clock controller base `MHT_CLK_BASE_REG = 0x0c800000`. CBCR bit 0 = enable,
bit 2 = clock reset, bit 31 = status. For each clock the config register is at
`base + rcg` (source select is 3 bits at bit 8) and the **command register that
commits it is 4 bytes below**, bit 0 = update, hardware clears it when done.

| register | meaning |
|---|---|
| `0x0c800004` / `0x0c800008` | switch core clock: RCG cfg / CBCR |
| `0x0c8001a4` | RCG feeding the four GEPHY and two SerDes sys clocks |
| `0x0c8001a8` / `0x0c8001ac` | SRDS0 / SRDS1 sys clock branches |
| `0x0c8001b0`..`0x0c8001bc` | GEPHY0..3 sys clock branches |
| `0x0c800058`..`0x0c800158` step `0x20` | bit 0 cleared, eight registers |
| `0x0c800304` | bits 0-3 = GEPHY P0-P3 **MDC software reset**, bit 4 = **DSP hardware reset** |
| `0x0c800308` | bit 0 = `GCC_TOP_FUNC_ARES`, the global reset (`MHT_GLOBAL_RST`) |
| `0x0c900000` | SKU fuse row |
| `0x0c900014` | fuse row (this port previously called it "the calibration gate") |
| `0x0c900048/5c/60/68` | calibration fuse rows |
| `0x0c90f014` | `SERDES_CFG` (this port previously called it "companion addresses") |
| `0x0c90f018` | `EPHY_CFG`, four 5-bit PHY addresses at bits 0/5/10/15 |
| `0x0c90f030` | `WORK_MODE`, mode field bits [5:0] |
| `0x0c90f03c` / `0x0c90f040` | `MDIO_CTRL0` / `MDIO_CTRL1` |
| `0x0c90f044` / `0x0c90f048` | `MEM_CTRL` / `MEM_ACC_0` |

Work mode values, from `mht_sec_ctrl.h`. Bits are PHY0_SEL 0, PHY1_SEL 1,
PHY2_SEL 2, PHY3_SEL0 3, PHY3_SEL1 4, PORT5_SEL 5:

```
MHT_SWITCH_MODE               0x10
MHT_SWITCH_BYPASS_PORT5_MODE  0x20
MHT_PHY_SGMII_UQXGMII_MODE    0x27
MHT_PHY_UQXGMII_MODE          0x2f
```

## What the driver now does, in order

All of it verified landing on hardware except where noted.

1. Log CMN PLL state, then pulse the package reset GPIO (tlmm gpio24, ACTIVE_LOW)
2. `bus->reset()`
3. Pulse the global reset, `0x0c800308` bit 0
4. Read `EPHY_CFG`, log strap addresses (reads `0x00318820` = 0,1,2,3)
5. `WORK_MODE` <- `MHT_SWITCH_MODE`
6. Commit the switch core RCG, enable and reset-pulse `0x0c800008`
7. Commit the GEPHY/SerDes sys RCG at `0x0c8001a4`
8. `MEM_CTRL` |= bit 5, `MEM_ACC_0` <- `0x000c0c0c`
9. Read SKU fuse, `MDIO_CTRL0/1`
10. `EPHY_CFG` low 20 bits <- `0x20c41` (addresses 1,2,3,4)
11. `SERDES_CFG` <- `0x1cc5` (5,6,7)
12. Enable + reset-pulse `0x0c8001a8`, `0x0c8001ac`
13. Clear bit 0 across `0x0c800058`..`0x0c800158`
14. Enable + reset-pulse `0x0c8001b0`..`0x0c8001bc`
15. Clear `0x0c800304` bits [4:0] (releases the four MDC resets and the DSP reset)
16. Calibration, gated on the fuse row, skipped on this silicon as on stock
17. Clear `EPHY_CFG` bits [21:20], settle 11 ms
18. Scan all 32 MDIO addresses

## Live values, for comparison

```
CMN PLL 9b064    0x00000000 at mdio probe (unlocked), 0x00000007 later (locked)
EPHY_CFG         0x00318820 -> 0x00020c41
WORK_MODE        0x0000000f -> 0x00000010          strap 0x0f is not a defined mode
0x0c800008       0x00004221                        switch core clock already enabled
0x0c800304       0x0000001f -> 0x00000000
MEM_CTRL         0x00000000, write of 0x20 DOES NOT LAND
MEM_ACC_0        0x00000000 -> 0x000c0c0c
SKU fuse         0x00000265, masked 0x00265        matches no known SKU
MDIO_CTRL0/1     0x00000000
GLOBAL_CTL       0x00000000
bus scan         only 0x10-0x1f answer, PHY ID varies with page state
                 (sometimes only 0x10; it changes with the switch core reset)
```

## Ruled out

- A write silently not landing. 27 of 28 writes read back correct.
- The MDIO protocol. Proven bidirectionally, many times.
- The chip being held in reset. Fixed; the GPIO is deasserted and logged.
- The CMN PLL being unmanaged. Fixed properly (patch 0069) and it now locks.
- Calibration. The fuse gate is genuinely false, so stock skips it too.
- The SoC-side ESS. qca-ssdk reports `Initializing SCOMPHY Done!!` for device 0.
- MHT support missing from the ssdk build. 303 MHT strings in the loaded module.
- The device tree shape. `ess-instance` has `num_devices = 2`, the SoC switch is
  `okay`, and the qca8386 node parses (`ssdk_dt_parse: switch node is qca8386!`).

## The two live anomalies worth chasing first

1. **`MEM_CTRL` refuses its write.** `0x0c90f044 <- 0x20` reads back `0x00000000`,
   both before and after the switch core clock was confirmed running. It is the
   only register on this board that has ever refused. The vendor comments that
   the dvs bits "need to be set one by one" for a hardware reason, which we do.
   Something gates this register and finding out what may be the whole answer.

2. **The SKU fuse reads `0x265`, and that is now confirmed real.** It was re-read
   the vendor's way, with the MDIO clock dropped to its slowest divider and then
   restored, exactly as `qca_mht_sku_check()` does. Both reads agree:

   ```
   SKU fuse at slow mdc (mode 0x0001503f -> 0x000150ff): row0 = 0x00000265, sku 0x00265
   SKU fuse at normal mdc:                               row0 = 0x00000265, sku 0x00265
   ```

   So it is not a clock artefact. `0x265` is none of QCA8082 `0x1dc`, QCA8084
   `0x1dd`, QCA8085 `0x1de` or QCA8386 `0x1df`. **This part is a variant the
   vendor driver does not enumerate.**

   It does not by itself block anything: `qca_mht_sku_switch_core_enabled()`
   returns false only for 8082/8084/8085, so an unrecognised SKU is treated as
   switch-capable. But it does mean the "this is a QCA8386" assumption rests on
   the vendor device tree rather than on the silicon, and every mode decision
   made so far inherits that. Worth weighing before spending more cycles on
   switch mode specifically.

   The neighbouring fuse row reads `row2 = 0x00080000`, which is the same value
   the calibration gate uses, so the fuse reads themselves are coherent.

## Other things to try

- `qca_mht_work_mode_init()` picks the mode from `mac_mode0`/`mac_mode1`. The strap
  is `0x0f`, which is no defined mode. Worth A/B testing `0x2f`
  (`MHT_PHY_UQXGMII_MODE`) against `0x10`, since in PHY mode the EPHYs are meant to
  be directly exposed, which is exactly the behaviour we want.
- The vendor U-Boot's `ipq_qca8084_work_mode_init` (`FUN_4a94d21c`, called from
  `FUN_4a94b918` at `0x4a94bb86`) reportedly fires ~13 reset pulses before writing
  the work mode. We pulse only 7 branches. Enumerate every CBCR in
  `mht_clk_lookup_table` and pulse them all.
- `board_eth_init` (`FUN_4a9267f0`) does nine phases of SoC-side clock and reset
  bring-up we do none of, across `0x01856xxx`, `0x01868xxx`, `0x01819xxx`,
  `0x01858xxx`, `0x01826xxx`, plus a switch reset recipe of `0x00098074 |= 2`,
  `mdelay(200)`, `0x00098480 |= 8`, TLMM pad `0x203`, reset low, **500 ms**,
  release. Ours is a 10 ms hold from the device tree. The full ordered list is in
  the git history of this file's commit "Transcribe the two vendor steps".
- Probe ordering: `90000.mdio` probes at 1.81 s, before the CMN PLL driver. Making
  preinit defer until the PLL is locked would be correct, though the rebind test
  below shows it does not change the outcome on its own.

## The free experiment loop

You do not need a build to re-run the whole bring-up. Unbinding and rebinding the
mdio driver re-runs `probe()`, and therefore preinit, at runtime:

```bash
ssh root@10.0.0.70 'echo 90000.mdio > /sys/bus/platform/drivers/ipq4019-mdio/unbind
                    echo 90000.mdio > /sys/bus/platform/drivers/ipq4019-mdio/bind'
dmesg | tail -40
```

This is how the CMN PLL was confirmed locked. Use it for anything that does not
need a code change; a build plus flash is about an hour.

## UART

An adapter is attached to the Mac as `/dev/cu.usbserial-110`, 115200. It is the
only way to see a boot that does not reach SSH, and it caught one flash that had
silently not completed.

```bash
screen /dev/cu.usbserial-110 115200
```

For logging, macOS ships screen 4.00.03 which has no `-Logfile`; use `-L` and it
writes `screenlog.0` in the working directory. A plain `cat` of the device does
**not** work: opening it resets the line to 9600 and you get garbage. If you script
it, hold the fd open across the `stty`:

```sh
exec 3<> /dev/cu.usbserial-110
stty -f /dev/cu.usbserial-110 115200 cs8 -cstopb -parenb raw -echo
exec cat <&3 >> console.log
```

## Ghidra

**Runs on `srv-openstack`, never on the Mac.** Five concurrent headless JVMs took
the MacBook's load average to 51 and made it unusable. The workspace is mirrored to
srv-openstack and verified there.

```bash
ssh srv-openstack 'docker run --rm -v ~/ghidra-workspace/project:/project \
  -v ~/ghidra-workspace/input:/input:ro -v ~/ghidra-workspace/scripts:/scripts:ro \
  --entrypoint /ghidra/support/analyzeHeadless blacktop/ghidra:latest \
  /project appsbl -process appsbl-mtd6.bin -noanalysis \
  -scriptPath /scripts -postScript FindEphyEnable.java'
```

Scripts must be `.java`. For parallel runs give each agent its own copy of the
project (`cp -R`, then delete the copy afterwards) since one project opens in one
process at a time. Strip output with
`sed 's/^INFO  Script.java> //; s/ (GhidraScript)  $//'`.

**The binary carries function-name strings** -- `ipq_qca8084_hw_init`,
`ipq_qca8084_work_mode_init`, `qca8084_switch_enable`, `_qca8084_interface_mode_init`,
`"uniphy callibration time out!"`, `"GMAC%d:Get QCA8084_PHY "`, `"cmbblk is stable %x"`.
Locate the string, find what references it, decompile that.

Key addresses: `0x4a94c4dc` read32, `0x4a94c524` write32, `0x4a94c35c` raw C22 write,
`0x4a94c3d0` raw C22 read, `0x4a94c630` clock/reset/calibrate, `0x4a94c7d0` MDIO
address assignment, `0x4a94b918` the caller of both, `0x4a9267f0` `board_eth_init`,
`0x4a94d21c` `ipq_qca8084_work_mode_init`, `0x4a94d694` reset-pulse-by-id.

## Build / flash / test loop

```bash
# full build, about 45 minutes
ssh srv-openstack 'cd ~/spnmx57/openwrt && \
  rm -rf build_dir/target-aarch64_cortex-a53_musl/linux-qualcommax_ipq50xx/linux-6.12.57 && \
  rm -f bin/targets/qualcommax/ipq50xx/*.bin && \
  nohup docker run --rm -u 1000 -v ~/spnmx57/openwrt:/build -w /build owrt-deb12 \
    nice -n 15 make -j24 > /tmp/buildN.log 2>&1 < /dev/null & disown'
```

**Compile-check first** with `make target/linux/compile V=sc -j24`. `V=sc` is the
only way to see a compiler error; a plain build just says "target/linux failed to
build".

**Patch generation is the single biggest time sink in this project.** The rule is
in the git history and it bit twice more tonight: generate the diff against a tree
with the *other* patches applied and *not* yours. The working method:

```bash
# remove your patch, re-prepare, then diff -- and assert the base is clean first
ssh srv-openstack 'cd ~/spnmx57/openwrt
  rm -f target/linux/qualcommax/patches-6.12/00NN-yours.patch
  rm -rf build_dir/.../linux-6.12.57
  docker run ... make target/linux/prepare -j24'
# only then copy the files out as the "a" side, edit "b", and diff -urN a b
```

`/tmp/wire_cmn3.py` on srv-openstack has a hard guard that refuses to build a diff
if the base already contains the change. Copy that pattern.

For the mdio patch specifically, always diff against `/tmp/mdio-ipq4019-PRISTINE.c`
and always re-apply the result to a fresh pristine copy to prove it round-trips.

**Flash as three separate commands**, never chained. A chained `&&` with a trailing
`&` silently does not run the upgrade, and a chained copy-flash-poll that hits a
tool timeout leaves the old image running while looking like a failed fix. Both
happened tonight.

```bash
scp -O build.bin root@10.0.0.70:/tmp/spnmx57.bin
ssh root@10.0.0.70 'sysupgrade -T -F /tmp/spnmx57.bin'
ssh root@10.0.0.70 'setsid sysupgrade -F -v /tmp/spnmx57.bin >/dev/null 2>&1 &'
```

Then **wait for uptime to drop** before reading dmesg, and confirm the new code is
live by a log line only it emits. `dmesg` right after issuing the flash shows the
*old* boot and reads exactly like a failed fix.

```bash
ssh root@10.0.0.70 'cut -d. -f1 /proc/uptime'   # poll until small
```

`CONFIG_DEVMEM=n`, no busybox `devmem`, no mdio-tools, so there is no userspace path
to these registers. Everything goes through the driver.

## Still open after Ethernet

- Re-enable `qca-nss-dp` once `chip_ver_get` succeeds
- Restore real `ethernet-phy` nodes for addresses 1-4 in a phy package container
- Upstream the `qca808x.c` 6.12 signature fix as its own PR
- **Upstream patch 0069** -- the CMN PLL driver being unbuildable is an OpenWrt bug
  affecting every IPQ5018 board, not just this one
- Parked: GL.iNet-style custom web UI
