# Handover: Linksys SPNMX57 OpenWrt Ethernet port

Repo: `~/Repositories/linksys-spnmx57-openwrt` -> `github.com/louij2/linksys-spnmx57-openwrt` (public)
Build host: `srv-openstack` (aarch64, 80 cores, 246 GB), tree `~/spnmx57/openwrt`, Docker image `owrt-deb12`.
**Ghidra runs on srv-openstack, never on the Mac.** Device is at **10.0.0.71**.

## Where things stand

Ethernet is most of the way up and does not yet pass packets.

| | |
|---|---|
| QCA8386 switch detected and initialised by qca-ssdk | yes |
| Four QCA8084 EPHYs answer MDIO at addresses 1-4, PHY ID `0x004dd180` | yes |
| Registered as Linux `phy_device`s, bound to "QCA808X SSDK ethernet" | yes |
| `lan` netdev created by qca-nss-dp, up, carrier, in `br-lan` forwarding | yes |
| **EPHY link with a live cable in a socket** | **no** |
| **Received packets** | **no — rx_packets 0, tx_packets climbing** |

Everything is currently configured for **1G**, deliberately. 2.5G comes after.

## THE CURRENT BLOCKER, AND THE NEXT THING TO DO

The four EPHYs are alive digitally and dead on the line side. Sampled 6 seconds
after bring-up, well past autonegotiation, with a cable in:

```
EPHY 1..4 at +6s: BMCR 0x1140  BMSR 0x7949  link down | STATUS 0x0000 link down
```

`BMCR 0x1140` = autoneg enabled, 1000/full, **not** powered down. `BMSR 0x7949`
advertises capabilities but never sets autoneg-complete or link.

**Leading theory: the QCA8084 analogue front end is never powered on.** The
vendor driver has explicit PLL and LDO enables that hang off port-enable /
PHY-power-on, which is driven by the phy being *started*. Nothing ever attaches
these phy_devices to a netdev — `lan` is bound to a fixed-link software PHY
instead — so that path never runs. That leaves the analogue side unpowered
while MDIO keeps working, which is exactly the symptom.

**These are plain MDIO debug-register writes and can go straight into preinit.**
The driver already has `qca8084_debug_write(bus, phy_addr, debug_reg, mask, set)`,
proven by the calibration code. From `src/hsl/phy/qca8084_phy.c` and
`include/hsl/phy/qca8084_phy.h`:

```
QCA8084_PHY_CONTROL_DEBUG_REGISTER0  0x1f    QCA8084_PHY_1588_P2_EN  0x0002
QCA8084_PHY_AFE25_CMN_6_MII          0x380   QCA8084_PHY_AFE25_PLL_EN 0x8000
QCA8084_PHY_AFE25_CMN_2_MII          0x180   QCA8084_PHY_AFE25_LDO_EN 0x2000
```

`qca8084_phy_pll_on(phy_addr)` is, per PHY:
```
debug 0x1f  |= 0x0002        (1588_P2_EN)
debug 0x380 |= 0x8000        (AFE25 PLL_EN)
mdelay(20)
```

`qca8084_phy_ldo_set(enable)` sets `debug 0x180 |= 0x2000` (LDO_EN) on **two**
PHYs only — `ephy_addr[1]` and `ephy_addr[2]`, i.e. the PHYs of mht ports 2 and
3, because LDO1/LDO2 are shared regulators driven from those two. It only does
this when all the *other* PHYs' PLLs are already on
(`qca8084_phy_pll_status_get` reads back `debug 0x380 & 0x8000`).

So the order to implement is: PLL on for all four, then LDO on for the middle
two, then re-sample link.

Also worth doing in the same build, all from `qca8084_phy_hw_init()`, all
commented in the vendor source as **link fixes**:
- `qca8084_phy_cdt_thresh_init()` — CDT thresholds
- `qca8084_phy_adc_edge_set(ADC_FALLING)` — "invert ADC clock edge as falling
  edge to fix link issue"
- MMD1 write `0x800a` = `0x51c6` — "configure signal energy detect threshold to
  fix link issue for some chips"
- `qca8084_phy_icc_init()`

## What is already ruled out

- **A write not landing.** Every write in the bring-up reads back correct.
- **The indirect protocol.** Fixed; see below. Proven bidirectionally.
- **The chip held in reset.** Fixed, and logged rather than assumed.
- **The CMN PLL unmanaged.** Fixed properly (patch 0069), it now locks.
- **Clock divider.** The GEPHY/SerDes sys RCG was already correct (src 0, div 3
  = 25 MHz from the 50 MHz XO) before we touched it. Verified by read-back.
- **Calibration.** Now un-gated and producing plausible distinct per-port trims
  (`0x1/0x2`, `0x0/0x2`, `0x0/0x4`, `0xf/0x1`). No link. Not the answer.
- **PHY driver binding.** All four bound to "QCA808X SSDK ethernet".
- **Switch forwarding / CPU port.** Not implicated: the PHYs themselves have no
  link, so nothing reaches the switch to forward.
- **1G vs 2.5G mismatch.** Dropping everything to 1G changed nothing.

## THE BUG THAT UNBLOCKED EVERYTHING

The 32-bit indirect register protocol was **dropping three address bits**:

```
bits [23:8]  page, written to phy 0x18 reg 0x0c
bits  [7:5]  select WHICH data pseudo-PHY: 0x10 | ((addr >> 5) & 7)
bits  [4:2]  the register pair within it, addr & 0x1c
```

The driver used a constant `phy 0x10`, so every access aliased into a 32-byte
window at the bottom of its page. Registers below offset `0x20` were correct by
luck, which is why parts of the sequence worked and made the addressing look
sound. Found by two measurements: a fuse-block dump repeating identically every
`0x20` bytes, and a write-mask probe showing `MEM_CTRL` writable only in
`[31:12]` — truncated, `0x44` is `0x04`, `GLOBAL_INTR_STATUS`, whose low bits
are read-only.

Fixing it made the EPHYs answer immediately.

## The other source that matters

**Stop reverse engineering the bootloader. qca-ssdk is the vendor's own Linux
driver and it ships in the build tree.**

```bash
ssh srv-openstack 'mkdir -p /tmp/ssdk && cd /tmp/ssdk && \
  tar --zstd -xf ~/spnmx57/openwrt/dl/qca-ssdk-2025.05.30~446db12b.tar.zst'
```

- `include/hsl/mht/mht_reg.h` — the entire QCA8084 register map, named
- `include/hsl/phy/qca8084_phy.h` — the PHY's own registers
- `src/hsl/phy/qca8084_phy.c` — PHY init, PLL, LDO, calibration
- `src/init/ssdk_mht_clk.c` — clock/reset table and how to drive it
- `src/init/ssdk_mht.c` — `qca_mht_hw_init`, the full bring-up order
- `src/init/ssdk_init.c` — `chip_ver_get` / `qca_detect_phyid`

It corrected two register names this port had guessed wrong and produced every
step that has worked since.

## Register map (from the vendor driver, not guessed)

Clock controller base `0x0c800000`. CBCR bit 0 = enable, bit 2 = clock reset,
bit 31 = status. Each clock's cfg register is at `base + rcg` (source select in
bits [10:8], divider `RCGR_HDIV` in bits [4:0]) and the **command register that
commits it is 4 bytes below**, bit 0 = update, hardware clears it when done.
Rate maths: `div = (prate * 2 / rate) - 1`.

| register | meaning |
|---|---|
| `0x0c800004` / `0x0c800008` | switch core clock RCG / CBCR (wants 312.5 MHz from src 1 = UNIPHY1_TX312P5M, which is not up early — leave it alone, the ssdk repoints it) |
| `0x0c8001a4` | RCG for the four GEPHY + two SerDes sys clocks (src 0 XO, div 3 = 25 MHz) |
| `0x0c8001a8` / `ac` | SRDS0 / SRDS1 sys clock branches |
| `0x0c8001b0`..`bc` | GEPHY0-3 sys clock branches |
| `0x0c800058`..`0x0c800158` step `0x20` | bit 0 cleared, eight registers |
| `0x0c800304` | bits 0-3 GEPHY P0-P3 MDC software reset, bit 4 DSP hw reset |
| `0x0c800308` | bit 0 global top-function reset (`MHT_GLOBAL_RST`) |
| `0x0c900000` | SKU fuse row — reads `0x265` |
| `0x0c900014` | fuse row (the "calibration gate") |
| `0x0c90f014` | `SERDES_CFG` |
| `0x0c90f018` | `EPHY_CFG`, four 5-bit PHY addresses at bits 0/5/10/15 |
| `0x0c90f030` | `WORK_MODE`, mode field bits [5:0] |
| `0x0c90f03c` / `40` | `MDIO_CTRL0` / `MDIO_CTRL1` |
| `0x0c90f044` / `48` | `MEM_CTRL` / `MEM_ACC_0` |

Work modes: `MHT_SWITCH_MODE 0x10`, `SWITCH_BYPASS_PORT5 0x20`,
`PHY_SGMII_UQXGMII 0x27`, `PHY_UQXGMII 0x2f`. Strap reads `0x3f`. The EPHYs
answer in **all** of them, so work mode is not what gates them.

**The SKU fuse reads `0x265`**, which is none of QCA8082 `0x1dc`, QCA8084
`0x1dd`, QCA8085 `0x1de` or QCA8386 `0x1df` — confirmed at the slowest MDIO
clock as well as the normal one, so not a clock artefact. This part is a variant
the vendor driver does not enumerate. It does not block anything
(`qca_mht_sku_switch_core_enabled()` returns false only for 8082/8084/8085) but
it means "this is a QCA8386" rests on the vendor device tree, not the silicon.

## What preinit does now, in order

1. Sample EPHY link "as found" (useful after a rebind, see below)
2. Deassert the package reset GPIO (tlmm gpio24, ACTIVE_LOW), `bus->reset()`
3. Pulse the global reset `0x0c800308` bit 0
4. Read `EPHY_CFG`, log strap addresses
5. Enable + reset-pulse the switch core CBCR (RCG left alone)
6. Commit the GEPHY/SerDes sys RCG (src 0, div 3)
7. Read SKU fuse at slow MDC, `MDIO_CTRL0/1`
8. `EPHY_CFG` low 20 bits <- `0x20c41` (addresses 1,2,3,4)
9. `SERDES_CFG` <- `0x1cc5` (5,6,7)
10. Enable + reset-pulse `0x0c8001a8`/`ac`, clear bit 0 across the eight
    `0x0c800058`.. registers, enable + reset-pulse `0x0c8001b0`..`bc`
11. Clear `0x0c800304` bits [4:0] (release MDC + DSP resets)
12. Per-port calibration (un-gated)
13. Clear `EPHY_CFG` bits [21:20], settle
14. `WORK_MODE` <- switch mode, then `MEM_CTRL`/`MEM_ACC_0` — **last, after
    every reset**, because the switch core reset pulse reverts work mode
15. Sample EPHY link post-init, +3s, +6s; full 32-address bus scan

## The free experiment loop

Unbind and rebind re-runs the whole of preinit at runtime, no build:

```bash
ssh root@10.0.0.71 'echo 90000.mdio > /sys/bus/platform/drivers/ipq4019-mdio/unbind
                    echo 90000.mdio > /sys/bus/platform/drivers/ipq4019-mdio/bind'
dmesg | tail -60
```

The "as found" sample at the top of preinit then reports the state the previous
session settled into, which is the only runtime view of PHY link this port has.

## Device tree, current state

- `&switch` (SoC ESS, device 0): `MAC_MODE_SGMII_CHANNEL0`, port@0 = port_id 1
  with mdio0 phy 7 (required for chip detection), port@1 = port_id 2 forced 1000.
  **`switch_cpu_bmp = <0>`, `switch_lan_bmp = <(ESS_PORT1 | ESS_PORT2)>`,
  `switch_wan_bmp = <0>`** — all three must be present or none are read, and
  without them `adpt_mp_port_netdev_change_notify()` bails before enabling the
  RX MAC.
- `&switch1` (QCA8386, device 1): `MAC_MODE_SGMII_CHANNEL0`, mac_mode1 disabled,
  cpu_bmp ESS_PORT0, lan_bmp ports 1-4, phy_address 1,2,3,4, port@0 forced 1000.
- `&mdio1`: `qcom,qca8084-preinit`, `qcom,qca8084-preinit-debug` (verbose; drop
  for a release), `qca8084-reset-gpios` (deliberately NOT `reset-gpios`), and
  four real `ethernet-phy@1..4` nodes.
- `&dp2`: `status okay`, `label = "lan"`, `fixed-link { speed 1000; full-duplex }`.
- `&mdio0`: must stay enabled or ssdk aborts.

Config additions in `config/qualcommax-config-additions`: `CONFIG_IPQ_CMN_PLL=y`
(needs patch 0069), `kmod-qca-ssdk`, `kmod-qca-nss-dp`.

## Known loose ends

- Generated bridge config lists ports `lan1 lan2 lan3`, which do not exist; the
  interface is `lan`. Set by hand on the device; needs fixing in board.d.
- `adpt_mp_port_netdev_change_notify: incorrect port 0` — the ssdk derives the
  port from `dev->phydev`, which is our fixed-link software PHY, so
  `qca_ssdk_phydev_to_port()` returns 0.
- **2.5G is blocked**: nss-dp always ends up with a phy_node (falls back to the
  dp node itself), so a fixed-link is mandatory, and `swphy_decode_speed()` only
  knows 10/100/1000 because Clause 22 cannot encode 2500. Restoring 2.5G means
  patching swphy or making nss-dp take its speed from the ssdk.
- Patch 0069 (CMN PLL never buildable) is an upstream OpenWrt bug affecting every
  IPQ5018 board and should be reported.

## Build / flash / test loop

```bash
# full build, ~45 min
ssh srv-openstack 'cd ~/spnmx57/openwrt && \
  rm -rf build_dir/target-aarch64_cortex-a53_musl/linux-qualcommax_ipq50xx/linux-6.12.57 && \
  rm -f bin/targets/qualcommax/ipq50xx/*.bin && \
  nohup docker run --rm -u 1000 -v ~/spnmx57/openwrt:/build -w /build owrt-deb12 \
    nice -n 15 make -j24 > /tmp/buildN.log 2>&1 < /dev/null & disown'
```

Compile-check first with `make target/linux/compile V=sc -j24`; `V=sc` is the
only way to see a compiler error.

**Patch generation is the biggest time sink here.** Always diff the mdio patch
against `/tmp/mdio-ipq4019-PRISTINE.c` on srv-openstack, never against
`build_dir` (which already has the patch applied), and always re-apply the
result to a fresh pristine copy to prove it round-trips. `/tmp/wire_cmn3.py` has
a hard guard that refuses to build a diff from a contaminated base — copy that
pattern. The working files live in `/tmp/work/{a,b}` on srv-openstack.

**Flash as separate commands, never chained**, and **the first flash often does
not take — check and repeat**:

```bash
scp -O build.bin root@10.0.0.71:/tmp/spnmx57.bin
ssh root@10.0.0.71 'sysupgrade -T -F /tmp/spnmx57.bin'
ssh root@10.0.0.71 'setsid sysupgrade -F -v /tmp/spnmx57.bin >/dev/null 2>&1 &'
```

Then **wait for uptime to drop** before reading dmesg, and confirm the new code
is live by a log line only it emits. `dmesg` right after issuing a flash shows
the *old* boot and reads exactly like a failed fix. This cost real time twice.

`CONFIG_DEVMEM=n`, no busybox `devmem`, no mdio-tools, so there is no userspace
path to these registers. Everything goes through the driver.

## UART

`/dev/cu.usbserial-110`, 115200, on the Mac. `screen /dev/cu.usbserial-110 115200`.
macOS screen is 4.00.03 with no `-Logfile`; use `-L`, it writes `screenlog.0` in
the cwd. A plain `cat` of the device gives garbage because opening it resets the
line to 9600 — hold the fd open across the `stty` if scripting it.

## Releases

- `v0.1.0-wifi-only` — everything but Ethernet, usable as a wireless repeater
- `v0.2.0-ethernet-bringup` — switch and PHYs up, no traffic yet

Luca wants, in order: **1G traffic end to end**, then a full write-up for the
OpenWrt forum (someone has asked) and to share the DTS, then 2.5G.
