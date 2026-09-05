## QCA8386 CPU-port SerDes (uniphy1) bring-up: MAC_MODE_SGMII_PLUS, forced 2500/full

All paths below are on the build host `srv-openstack`. SSDK root = `/tmp/ssdk/qca-ssdk-2025.05.30~446db12b/`; OpenWrt patches root = `/tmp/owrt-main/target/linux/qualcommax/patches-6.18/`.

The authoritative code path for the CPU uplink is: `mht_interface_mac_mode_set(port0)` → `mht_interface_sgmii_mode_set(uniphy_index=MHT_UNIPHY_SGMII_1, port0, config)` → `mht_uniphy_sgmii_function_reset(SGMII_1)`. The board wants `mac_mode=FAL_MAC_MODE_SGMII_PLUS`, `clock_mode=FAL_INTERFACE_CLOCK_MAC_MODE`, `auto_neg=false`, `force_speed=FAL_SPEED_2500` — proven by the CPU-port config builder `_qca_mht_interface_mode_init` at `src/init/ssdk_mht.c:72-112` (SGMII_PLUS at :90-92, MAC clock at :106, `auto_neg=!force_en` at :107, force_speed from `hsl_port_force_speed_get` at :88-90) and by the SPNMX57 DTS `switch_mac_mode = <MAC_MODE_SGMII_PLUS>` + `port@0 forced-speed=<2500>` (`/home/luca/spnmx57/openwrt/target/linux/qualcommax/files/arch/arm64/boot/dts/qcom/ipq5018-spnmx57.dts:53,63-66,116-117`).

---

### (1) How the switch-internal PCS/SerDes is ADDRESSED over the indirect MDIO

Two distinct addressing layers are involved — do not confuse them:

**Layer A — the switch register window (32-bit indirect MDIO), used only to *discover* the serdes MDIO address.**
`qca_mht_serdes_addr_get` (`src/hsl/mht/mht_sec_ctrl.c:60-84`) does `qca_mht_mii_read(dev_id, SERDES_CFG_OFFSET)` and extracts a 5-bit MDIO PHY address from a bitfield:
- `MHT_UNIPHY_SGMII_0` → `(data >> SERDES_CFG_S0_ADDR_BOFFSET) & 0x1f` (bit offset 0)
- `MHT_UNIPHY_SGMII_1` → `(data >> SERDES_CFG_S1_ADDR_BOFFSET) & 0x1f` (bit offset 5)  ← **CPU uplink**
- `MHT_UNIPHY_XPCS` → `(data >> SERDES_CFG_S1_XPCS_ADDR_BOFFSET) & 0x1f` (bit offset 10)

`SERDES_CFG_OFFSET = 0xC90F014`, `S1_XPCS=10`, `S1=5`, `S0=0` (`include/hsl/mht/mht_reg.h:165,172,178,184`). `qca_mht_mii_read`/`_write` are `#define`d to `qca_mii_read`/`qca_mii_write` (`include/init/ssdk_plat.h:517-518`), whose bodies are at `src/init/ssdk_plat.c:372-405`. Before the MDIO transaction, `qca_mii_reg_convert` (`src/init/ssdk_plat.c:276-289`) ORs in `SSDK_SWITCH_REG_TYPE_QCA8386` (= FIELD_PREP(mask,0), `include/init/ssdk_plat.h:389`) to tag the register as a QCA8386 (not QCA8337) window access. The actual page-split write (your RE'd "page → pseudo-PHY 0x18 reg 0x0c, 32-bit split across two 16-bit MDIO cycles") is done by the `mdio_priv->sw_read/sw_write` callback (`qca_mii_raw_read/write`, `src/init/ssdk_plat.c:291-312`) supplied by the registered `mii_bus` — that glue is in the kernel qca8k/mdio layer, not in this SSDK tree. So SERDES_CFG at register 0xC90F014 is itself read through the qca8k-style indirect window.

**Layer B — the serdes/uniphy itself is a normal clause-45 MDIO device** at the 5-bit address just discovered, on the *same* MDIO bus (`&mdio1` in the DTS, `ipq5018-spnmx57.dts:49,232`). Every register write below goes through `hsl_phy_mmd_reg_write`/`hsl_phy_modify_mmd`/`hsl_phy_modify_mii` with `is_c45 = A_TRUE`:
- MMD accesses take `HSL_PHY_REG_C45_ADDR(mmd_num, reg) = BIT(30) | mmd_num<<16 | reg` (`include/hsl/phy/hsl_phy.h:872`), and the `BIT(30)` (`SSDK_ADDR_C45`, `include/init/ssdk_plat.h:258`) routes to `__mdiobus_c45_write(bus, phy_addr, devad, reg, val)` (`src/hsl/phy/hsl_phy.c:3042-3051`). So these are genuine IEEE clause-45 frames (devad + 16-bit reg), NOT the clause-22 MMD13/14 indirection.
- MII accesses (`hsl_phy_modify_mii`, regs 0 and 6) are plain clause-22 reads/writes to the same phy_addr.

**MMD device numbers:** `MHT_UNIPHY_MMD1 = 0x1` = `MDIO_MMD_PMAPMD` (analog/mode/calibration/reset), `MHT_UNIPHY_MMD3 = 0x3` = `MDIO_MMD_PCS` (10G-BaseR/XPCS), per `include/hsl/mht/mht_interface_ctrl.h:29-30` and kernel `linux/mdio.h:18,21`. The per-channel XPCS MMDs are `MMD31/26/27/28` for channels 0/1/2/3 (`mht_interface_ctrl.h:31-34`, `mht_uniphy_xpcs_port_to_mmd` at `mht_interface_ctrl.c:94-118`).

**Net for the CPU port at SGMII+ 2500:** address = `qca_mht_serdes_addr_get(MHT_UNIPHY_SGMII_1)` (SERDES_CFG bits [9:5]); MMD device = **PMAPMD (1)** for every register write, plus clause-22 MII regs 0 and 6. The XPCS address (bits [14:10]) and MMD3/PCS are **not** touched in the SGMII+ path — they belong to the 10G-QXGMII mode only.

---

### (2) ORDERED register writes for SGMII+ forced 2500/full

Register/field values from `include/hsl/mht/mht_interface_ctrl.h` (lines cited). For the SGMII+ MAC-mode forced-2500 case: `mode_ctrl = MHT_UNIPHY_MMD1_SGMII_PLUS_MODE(0x800) | MHT_UNIPHY_MMD1_SGMII_MAC_MODE(0x20) = 0x820`; `raw_clk = UNIPHY_CLK_RATE_312M = 312500000`. Source: `mht_interface_sgmii_mode_set`, `src/hsl/mht/mht_interface_ctrl.c:585-740`.

Pre-step (clock, Layer A / NSSCC — via switch window, maps to Linux clk in phylink):
| # | Op | Target | Value / field | Meaning | Cite |
|---|----|--------|---------------|---------|------|
| 0a | clk deassert | `MHT_SRDS1_SYS_CLK` | — | ungate uniphy1 system clock if asserted | `mht_interface_ctrl.c:600-604` |
| 0b | clk assert | `MHT_UNIPHY_XPCS_RST` | — | hold XPCS in reset (SGMII+ does not use XPCS) | `:655` |
| 0c | raw-clk set | `MHT_P_UNIPHY1_RX`, `MHT_P_UNIPHY1_TX` | `312500000` | uniphy1 rx/tx line rate = 312.5 MHz (the 2.5× that turns 1G-SGMII signalling into 2.5G) | `:657-658`; rate const `include/init/ssdk_clk.h:304`; also `mht_port_speed_clock_set`→`UQXGMII_SPEED_2500M_CLK=312500000` `mht_interface_ctrl.c:143-144`, `include/init/ssdk_mht_clk.h:162` |
| 0d | port GMII/uniphy clk | port0 + `MHT_CLK_TYPE_UNIPHY` | disable | quiesce GMII/uniphy clocks before reconfig | `:636-649` |

Uniphy register sequence (Layer B — clause-45 to the SGMII_1 address; MII = clause-22):
| # | Bus op | MMD / reg | Mask | Value | Meaning | Cite |
|---|--------|-----------|------|-------|---------|------|
| 1 | modify_mii | reg 6 `PLL_LOOP_CONTROL` | `0x30` `CML2CMS_IBSEL` | `0x30` | PLL bias fix for high-temp lock stability | `:667-670`; defs `mht_interface_ctrl.h:21,26` |
| 2 | modify_mmd | **MMD1** `MODE_CTRL` 0x11b | `0x1f70` `SGMII_MODE_CTRL_MASK` | `0x820` (SGMII_PLUS 0x800 \| MAC 0x20) | select SGMII+ mode, uniphy as MAC | `:671-675`; defs `mht_interface_ctrl.h:40,49,52,53` |
| 3 | modify_mmd | **MMD1** `GMII_DATAPASS_SEL` 0x180 | `0x1` `DATAPASS_MASK` | `0x0` `DATAPASS_SGMII` | route GMII datapath as SGMII (not USXGMII) | `:676-680`; defs `mht_interface_ctrl.h:42,60-62` |
| 4 | modify_mmd | **MMD1** `CHANNEL0_CFG` 0x120 | `0xe` `CH0_FORCE_SPEED_MASK` | `0xc` (FORCE_ENABLE 0x8 \| FORCE_SPEED_1G 0x4) | force speed, disable AN. NOTE: 2500 and 1000 BOTH write the "1G" code 0x4 — the 2.5× comes from mode=SGMII_PLUS + 312.5 MHz raw clock, not from this field | `:682-713` (2500 case :697-701); defs `mht_interface_ctrl.h:41,54-59` |
| 5 | clk reset | port0 GMII + uniphy clocks | — | reset/release the GMII+uniphy interface clocks | `:714-719` |
| 6 | modify_mii | reg 0 `PLL_POWER_ON_AND_RESET` | `0x40` | `0x00` `ANA_SOFT_RESET` | assert analog soft-reset | `:722-724`; defs `mht_interface_ctrl.h:20,24` |
| 7 | delay | — | — | `mdelay(1)` | (see timings) | `:725` |
| 8 | modify_mii | reg 0 `PLL_POWER_ON_AND_RESET` | `0x40` | `0x40` `ANA_SOFT_RELEASE` | release analog soft-reset | `:726-728`; def `mht_interface_ctrl.h:25` |
| 9 | poll | **MMD1** `CALIBRATION4` 0x78 | `0x80` `CALIBRATION_DONE` | wait set | wait uniphy calibration done | `:729-731`; `mht_uniphy_calibration` `:205-227` |
| 10 | clk enable | port0 GMII + uniphy clocks | — | re-enable interface clocks | `:732-737` |

SGMII function reset (`mht_uniphy_sgmii_function_reset(SGMII_1)`, called after mode set from `mht_interface_mac_mode_set:792`; body `:379-407`):
| # | Bus op | MMD / reg | Mask | Value | Meaning | Cite |
|---|--------|-----------|------|-------|---------|------|
| 11 | modify_mmd | **MMD1** `CHANNEL0_CFG` 0x120 | `0x800` `SGMII_ADPT_RESET` | `0` | assert SGMII ch0 adapter reset | `:388-391`; def `mht_interface_ctrl.h:65` |
| 12 | delay + set | **MMD1** 0x120 | `0x800` | `0x800` | `mdelay(1)` then de-assert adapter reset | `:392-396` |
| 13 | modify_mmd | **MMD1** `USXGMII_RESET` 0x18c | `0x10` `SGMII_FUNC_RESET` | `0` | assert SGMII function/IPG-tune reset | `:397-400`; def `mht_interface_ctrl.h:43,64` |
| 14 | delay + set | **MMD1** 0x18c | `0x10` | `0x10` | `mdelay(1)` then de-assert function reset | `:401-404` |

That is the complete forced-2500 SGMII+ bring-up. (SSCG enable and CDR/SSC-fix are NOT done in the SGMII+ path — they appear only in the UQXGMII path at `:459-460`, and in mainline `CDR_CONTRL`/`SSC_FIX_MODE` only inside `qca8084_pcs_set_mode`.)

---

### (3) Polling / reset steps and their timings

- **Analog soft-reset pulse** (steps 6-8): assert 0, `mdelay(1)` (`mht_interface_ctrl.c:725`), release. Note the UQXGMII path uses `mdelay(10)` for the same pulse (`:449`), and mainline `qca8084_do_calibration` uses `usleep_range(10000,11000)` (10-11 ms) — so 10 ms is the safer value to port; 1 ms is what the SGMII path currently uses.
- **Calibration poll** (step 9): loop up to `retries=100`, `mdelay(1)` per iteration → ~100 ms max, poll MMD1 reg 0x78 bit 0x80 (`mht_uniphy_calibration`, `:209-224`). Mainline equivalent: `read_poll_timeout(...,100,100000,...)` = 100 µs poll interval, 100 ms timeout (`0940-*.patch`, `qca8084_do_calibration`).
- **SGMII adapter + function reset pulses** (steps 11-14): each is assert → `mdelay(1)` → de-assert (`:392,:401`). Mainline `qca8084_pcs_ipg_tune_reset` uses `usleep_range(1000,1100)` (`0941-*.patch`).
- **XPCS reset** is *asserted and left asserted* for SGMII+ (`ssdk_mht_clk_assert(MHT_UNIPHY_XPCS_RST)`, `:655`); no XPCS soft-reset poll in this path.
- No 10G-BaseR link poll and no XPCS soft-reset poll in SGMII+ (those are UQXGMII-only: `mht_uniphy_xpcs_10g_r_linkup` `:229-251` with retries=100/1ms, and `mht_uniphy_xpcs_soft_reset` `:253-276` with retries=100/1ms).

For a phylink port, the reset/calibration polls (steps 6-9, 11-14) live in `pcs_config`/`mac_prepare`; the clock rate (step 0c, 312500000) is a `clk_set_rate` on the uniphy1 rx/tx clocks.

---

### (4) Relationship: PHY-package XPCS/PCS serdes vs the switch CPU-port uplink serdes

**They are the same silicon IP block (the Qualcomm "uniphy"/Napa serdes + XPCS/PCS), driven through the same register map — just reached and clocked differently, and in the SPNMX57's case configured for a different mode.**

Proof of identical register model — the mainline `qca8084_serdes.c` (added by `0940-net-phy-qca808x-Add-QCA8084-SerDes-init-function.patch`) and the SSDK MHT uniphy code define the *same* registers at the *same* MMDs with the *same* field encodings:

| Meaning | SSDK (`mht_interface_ctrl.h`) | Mainline (`0940-*.patch`) | Same? |
|---|---|---|---|
| Mode ctrl reg | `MMD1 MODE_CTRL 0x11b` | `MDIO_MMD_PMAPMD MODE_CONTROL 0x11b` | yes (PMAPMD=MMD1) |
| SGMII+ select | `SGMII_PLUS_MODE = 0x800` | `FIELD_PREP(GENMASK(12,8), MODE_CONTROL_SGMII_PLUS=0x8)` = 0x800 | yes |
| SGMII select | `SGMII_MODE = 0x400` | `SGMII=0x4` → 0x400 | yes |
| XPCS select | `XPCS_MODE = 0x1000` | `XPCS=0x10` → 0x1000 | yes |
| MAC / PHY clk | `MAC=0x20 / PHY=0x10` | `SGMII_MAC=2 / SGMII_PHY=1` in GENMASK(6,4) → 0x20 / 0x10 | yes |
| Datapass | `GMII_DATAPASS_SEL 0x180`, SGMII=0 | `QP_USXG_OPTION1 0x180`, DATAPASS bit0=0 | yes |
| Calibration | `MMD1 CALIBRATION4 0x78`, done 0x80 | `PMAPMD CALIBRATION4 0x78`, `CALIBRATION_DONE BIT(7)` | yes |
| Ana reset | MII reg0 bit 0x40 | `PLL_POWER_ON_AND_RESET 0x0`, `PCS_ANA_SW_RESET BIT(6)` | yes |
| PLL bias | MII reg6 mask 0x30 | `PLL_CONTROL 6`, `CMLDIV2_IBSEL GENMASK(5,4)` | yes |
| Func reset | `USXGMII_RESET 0x18c`, `SGMII_FUNC_RESET 0x10` | `QP_USXG_RESET 0x18c`, `QP_USXG_SGMII_FUNC_RESET BIT(4)` | yes |
| Per-channel MMDs | 31/26/27/28 | `qca8084_xpcs_ch_mmd[] = {31,26,27,28}` | yes |
| 2500 raw clock | `312500000` | `SPEED_2500 → rate 312500000` | yes |

Crucially, mainline's `qca8084_pcs_set_interface_mode` (`0940-*.patch`) already handles `PHY_INTERFACE_MODE_2500BASEX → MODE_CONTROL_SGMII_PLUS` with `DATAPASS_SGMII` — i.e. it knows the exact SGMII+ case you need — even though the patch series as-merged only *calls* it with `PHY_INTERFACE_MODE_10G_QXGMII` (from `qca8084_pcs_set_mode`, invoked only when `phydev->interface == PHY_INTERFACE_MODE_10G_QXGMII`, `qca808x.c` hunk in `0940`).

**How they differ:**
1. **Role/instances.** The QCA8386 die contains two of these serdes instances plus the shared XPCS: `MHT_UNIPHY_SGMII_0` (→ external port5), `MHT_UNIPHY_SGMII_1` (→ port0, the CPU uplink), and `MHT_UNIPHY_XPCS` (the 10G-QXGMII PCS that fans uniphy1 out to the 4 internal QCA8084 EPHYs). Each has its own MDIO address in SERDES_CFG (`mht_sec_ctrl.c:70-78`). The standalone QCA8084 *PHY package* exposes the same blocks as two OF-described mdio devices, `pcs-phy` (= the PMAPMD/MMD1 analog+mode side = "SGMII" address) and `xpcs-phy` (= the MMD3/PCS + per-channel side = "XPCS" address), probed in `0939-*.patch` (`qca8084_package_pcs_probe` / `qca8084_package_xpcs_probe`).
2. **Addressing.** Switch integration: serdes MDIO address is *read at runtime* from the switch's SERDES_CFG register via the qca8k indirect window (Layer A above). Package: addresses are fixed by devicetree `pcs-phy`/`xpcs-phy` nodes on the package MDIO bus.
3. **Clock/reset plumbing.** Switch: `ssdk_mht_clk_*` names (`MHT_SRDS1_SYS_CLK`, `MHT_UNIPHY_XPCS_RST`, `MHT_P_UNIPHY1_RX/TX`) programmed through the switch register window's internal GCC (`src/init/ssdk_mht_clk.c`). Package: standard Linux `clk`/`reset` framework against the package NSSCC (`enum xpcs_clk_id`/`pcs_clk_id`, `devm_clk_get`, `reset_control_*` in `0939`/`0940`/`0941`).
4. **Mode actually used on SPNMX57.** The CPU uplink runs **plain 1-lane SGMII+ 2500** on uniphy1 (MMD1/PMAPMD only, XPCS held in reset). The mainline package driver as-shipped wires up only **10G-QXGMII** (MMD3/PCS BaseR + XPCS + 4 channel MMDs: `qca8084_xpcs_set_mode`/`qca8084_qxgmii_set_mode` in `0940`, per-channel speed in `qca8084_qxgmii_set_speed` `0941`). Same serdes, different multiplexing: in SGMII+ it is a single 2.5G lane to the SoC MAC; in QXGMII the XPCS runs 10G-BaseR and time-division-multiplexes 4 sub-channels to the 4 PHYs.

**Porting implication:** the register writes in section (2) are exactly the `qca8084_pcs_set_interface_mode(PHY_INTERFACE_MODE_2500BASEX)` branch (mode=SGMII_PLUS, datapass=SGMII) plus PLL-bias, analog reset, calibration poll, and the SGMII adapter/function resets — all on MMD1/PMAPMD of the uniphy1 (SGMII_1) address, with rx/tx clk at 312.5 MHz. You do NOT need the MMD3/PCS BaseR, USXGMII_EN, QXGMII_EN, AM-interval, XPCS soft-reset, or per-channel MMD writes (those are QXGMII-only). If the QCA8386 is modelled as a qca8k-style DSA switch, this sequence belongs in the CPU (port0) `phylink_pcs` `pcs_config`/`pcs_link_up` (or the DSA `port_config` for a fixed-link CPU port), with the serdes reached by resolving SERDES_CFG[9:5] to a clause-45 mdio_device on the switch's MDIO bus.