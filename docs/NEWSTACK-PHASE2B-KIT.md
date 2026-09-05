# SPNMX57 Phase 2b implementation kit — QCA8386 DSA driver (`qca8386.c`) + device tree

**Target:** write `drivers/net/dsa/qca/qca8386.c` (forked from mainline v6.18 `qca8k`) plus the SPNMX57 device-tree, to drive the QCA8386 switch on the IPQ5018-based SPNMX57 board: CPU port 0 uplinked to the SoC MAC1 at forced 2.5G SGMII+, four user ports (integrated QCA8084-family EPHYs) at MDIO 1..4.

**One-chip mental model (settled from the four sections).** The QCA8386 ("MHT" / Manhattan) is a single package that integrates: a qca8k-family L2 switch core (7 ports, 0..6), two "uniphy" SerDes instances plus a shared XPCS, four QCA8084 EPHYs, and an NSS clock/reset controller ("nsscc"). All sub-blocks are addressed on the SoC's `mdio1` bus. "External QCA8084s" in the fork map means external to the IPQ5018 SoC, not a second chip. On SPNMX57 the SoC talks to the switch over one link (CPU port 0, SGMII+ 2.5G) and the switch bridges internally to the four EPHYs. gpio24 is the package reset for the whole QCA8386.

**Source hosts / trees (all refs below are verbatim from the four research passes):**
- SSDK: `srv-openstack:/tmp/ssdk/qca-ssdk-2025.05.30~446db12b/`
- OpenWrt patches: `/tmp/owrt-main/target/linux/qualcommax/patches-6.18/`
- Mainline v6.18 fetched copies: `srv-openstack:/tmp/qca8k.h`, `/tmp/nsscc-qca8k.c`, plus `qca8k-8xxx.c`/`qca8k-common.c`/`qca8k.h`
- Authoritative board DTS (old stack): `ipq5018-spnmx57.dts` (full path `srv-openstack:/home/luca/spnmx57/openwrt/target/linux/qualcommax/files/arch/arm64/boot/dts/qcom/ipq5018-spnmx57.dts`)

> Note: git.kernel.org returns HTTP 403 to WebFetch; the mainline files were fetched with `curl` from `srv-openstack`.

---

## 1. MHT (QCA8386) register map + C defines

**Key structural finding (read first):** the QCA8386 switch-core L2/port block is register-compatible with the QCA8337 (ssdk "isisc") family and is NOT defined in `mht_reg.h`. `mht_reg.h` carries only the MHT-specific glue (GCC clock/reset, SerDes/UNIPHY config, EPHY address mapping, PTP mux, the pseudo-PHY MDIO master `MDIO_CTRL0/1`, NAT/route/flow blocks). It does not define `MASK_CTRL`, `PORT_STATUS`, `PORT_HDR_CTL`, `FORWARD_CTL0/1`, or `PORT_LOOKUP_CTL`.

Proof:
- every MHT switch-core C file includes `isisc_reg.h`: `mht_init.c:31` `#include "isisc_reg.h"` (also `mht_port_ctrl.c`, `mht_sec_ctrl.c`, `mht_ip.c`, `mht_nat.c`)
- `grep MASK_CTRL|PORT_STATUS|... mht_reg.h` -> 0 hits (only `MHT_DEVICE_ID 0x17` at `mht_reg.h:26`)
- the requested registers live in `/tmp/ssdk/qca-ssdk-2025.05.30~446db12b/include/hsl/isisc/isisc_reg.h`

**Consequence:** for the DSA port the mainline `qca8k` register layer applies almost verbatim. The QCA8386 delta is entirely in the SerDes/UNIPHY 2.5G bring-up (section 2), the indirect MDIO window page register (0x0c, section 4), and identity (device id 0x17), not in the switch-core registers.

### 1.1 MASK_CTRL — chip id / revision (offset 0x0000)
Source `isisc_reg.h:36` (`MASK_CTL_OFFSET 0x0000`), fields `:46-63`; device id `MHT_DEVICE_ID 0x17` `isisc_reg.h:25` / `mht_reg.h:26`.

| offset | field | bits | meaning | qca8k mapping |
|---|---|---|---|---|
| 0x0000 | SOFT_RST | [31] | full switch soft reset (RW) | not in qca8k (resets via GCC) — MHT/isisc only |
| 0x0000 | LOAD_EEPROM | [16] | EEPROM reload (RW) | not in qca8k |
| 0x0000 | DEVICE_ID | [15:8] | chip id, reads 0x17 for QCA8386 | identical: `QCA8K_MASK_CTRL_DEVICE_ID_MASK GENMASK(15,8)` (qca8k.h:45) |
| 0x0000 | REV_ID | [7:0] | silicon revision (RO) | identical: `QCA8K_MASK_CTRL_REV_ID_MASK GENMASK(7,0)` (qca8k.h:43) |

qca8k `QCA8K_REG_MASK_CTRL 0x000` (qca8k.h:42). Bind the driver on device id **0x17**.

### 1.2 PORT_STATUS — per-port MAC force/link/speed (0x007c + port*4)
Source `isisc_reg.h:1012-1076`; stride `E_OFFSET 0x0004`, `NR_E 7`. qca8k `QCA8K_REG_PORT_STATUS(_i) (0x07c + (_i)*4)` (qca8k.h:137) — identical offset+stride.

| field | bits | meaning | qca8k mapping |
|---|---|---|---|
| FLOW_LINK_EN | [12] | 1=flow follows AN; 0=forced TX/RX flow | `QCA8K_PORT_STATUS_FLOW_AUTO BIT(12)` (qca8k.h:150) |
| AUTO_RX_FLOW | [11] | RO resolved RX pause | bit11 undefined in qca8k |
| AUTO_TX_FLOW | [10] | RO resolved TX pause | `QCA8K_PORT_STATUS_LINK_PAUSE BIT(10)` (qca8k.h:149) |
| LINK_EN | [9] | force-mode enable: 1=MAC follows PHY, 0=MAC uses forced fields | `QCA8K_PORT_STATUS_LINK_AUTO BIT(9)` (qca8k.h:148). Clear to force the CPU port. |
| LINK | [8] | RO link up | `QCA8K_PORT_STATUS_LINK_UP BIT(8)` (qca8k.h:147) |
| TX_HALF_FLOW_EN | [7] | half-duplex backpressure | not in qca8k |
| DUPLEX_MODE | [6] | forced duplex 1=full | `QCA8K_PORT_STATUS_DUPLEX BIT(6)` (qca8k.h:146) |
| RX_FLOW_EN | [5] | forced RX pause | `QCA8K_PORT_STATUS_RXFLOW BIT(5)` (qca8k.h:145) |
| TX_FLOW_EN | [4] | forced TX pause | `QCA8K_PORT_STATUS_TXFLOW BIT(4)` (qca8k.h:144) |
| RXMAC_EN | [3] | RX MAC enable | `QCA8K_PORT_STATUS_RXMAC BIT(3)` (qca8k.h:143) |
| TXMAC_EN | [2] | TX MAC enable | `QCA8K_PORT_STATUS_TXMAC BIT(2)` (qca8k.h:142) |
| SPEED_MODE | [1:0] | forced speed code | `QCA8K_PORT_STATUS_SPEED GENMASK(1,0)` (qca8k.h:138) |

There are no separate "force speed/duplex/flow enable" bits. Force exactly as qca8k does: clear LINK_AUTO (bit 9), then write SPEED_MODE, DUPLEX_MODE, flow, and MAC enables directly (`mht_port_ctrl.c:_mht_port_mac_speed_set`, SPEED_MODE write at `:546`).

**SPEED encoding — how 2500 is handled (load-bearing):** the 2-bit field is NOT widened. 2.5G reuses the 1000M code (value 2):
- `mht_port_ctrl.h:28-31`: `MHT_PORT_SPEED_10M 0`, `_100M 1`, `_1000M 2`, `_2500M == _1000M (== 2)`
- set path `mht_port_ctrl.c:539-546`: `FAL_SPEED_1000 -> 2` and `FAL_SPEED_2500 -> MHT_PORT_SPEED_2500M (also 2)`
- get path `mht_port_ctrl.c:718-732`: decodes 0->10, 1->100, 2->1000; **no case for 2500, no field==3**. The switch-core MAC cannot distinguish 2500 from 1000.

**Driver rule:** to bring the CPU port up at 2.5G, write `SPEED_MODE = 2` (the same value as `QCA8K_PORT_STATUS_SPEED_1000`, 0x2), DUPLEX=1, MAC enables on, LINK_AUTO=0. The 2500 rate comes entirely from the SerDes (section 2). qca8k has only 10/100/1000 codes (`qca8k.h:139-141`), so no PORT_STATUS source change is needed for the speed value; do NOT invent a distinct 2500 code and do NOT widen the mask. (See cross-check C-3, which reconciles this against section 4's "widen/verify" note.)

### 1.3 HEADER_CTL + PORT_HDR_CTL (0x0098 / 0x009c)
Source `isisc_reg.h:1082-1126`.

HEADER_CTL (global, 0x0098):

| field | bits | meaning | qca8k mapping |
|---|---|---|---|
| TYPE_LEN | [16] | header carries a length | no qca8k define (tag_qca uses fixed 2-byte header, never programs an ethertype) |
| TYPE_VAL | [15:0] | header ethertype value | not used by qca8k |

PORT_HDR_CTL (per-port, 0x009c + port*4, NR_E 7):

| field | bits | meaning | qca8k mapping |
|---|---|---|---|
| IPG_DEC_EN | [5] | shrink IPG when header added | not in qca8k |
| LOOPBACK_EN | [4] | port loopback | not in qca8k |
| RXHDR_MODE | [3:2] | 0=none,1=mgmt,2=all | `QCA8K_PORT_HDR_CTRL_RX_MASK GENMASK(3,2)` (qca8k.h:152) |
| TXHDR_MODE | [1:0] | 0=none,1=mgmt,2=all | `QCA8K_PORT_HDR_CTRL_TX_MASK GENMASK(1,0)` (qca8k.h:153) |

Mode values match qca8k (`NONE 0`, `MGMT 1`, `ALL 2`, qca8k.h:154-156). CPU port TX+RX = ALL(2); user ports NONE(0). Register `QCA8K_REG_PORT_HDR_CTRL(_i) (0x9c + (_i*4))` (qca8k.h:151).

### 1.4 FORWARD_CTL0 (= GLOBAL_FW_CTRL0) — CPU port enable (0x0620)
Source `isisc_reg.h:1787-1846`.

| field | bits | meaning | qca8k mapping |
|---|---|---|---|
| CPU_PORT_EN | [10] | master enable for the CPU port | `QCA8K_GLOBAL_FW_CTRL0_CPU_PORT_EN BIT(10)` (qca8k.h:245) |
| MIRROR_PORT_NUM | [7:4] | monitor/mirror dest port | `QCA8K_GLOBAL_FW_CTRL0_MIRROR_PORT_NUM GENMASK(7,4)` (qca8k.h:246) |

Other isisc bits (`:1794-1866`, ARP_CMD, HASH_MODE, NAT drops, IGMP, PPPOE_RDT_EN etc.) are unused by DSA. qca8k `QCA8K_REG_GLOBAL_FW_CTRL0 0x620` (qca8k.h:244).

### 1.5 FORWARD_CTL1 (= GLOBAL_FW_CTRL1) — flood dest-port bitmaps (0x0624)
Source `isisc_reg.h:1874-1898`. Each field is a 7-bit port bitmap (bit N = port N).

| field | bits | qca8k mapping |
|---|---|---|
| IGMP_DP | [30:24] | `QCA8K_GLOBAL_FW_CTRL1_IGMP_DP_MASK GENMASK(30,24)` (qca8k.h:248) |
| BC_FLOOD_DP | [22:16] | `..._BC_DP_MASK GENMASK(22,16)` (qca8k.h:249) |
| MUL_FLOOD_DP | [14:8] | `..._MC_DP_MASK GENMASK(14,8)` (qca8k.h:250) |
| UNI_FLOOD_DP | [6:0] | `..._UC_DP_MASK GENMASK(6,0)` (qca8k.h:251) |

To flood unknowns to the CPU set bit 0 in the relevant field. qca8k `QCA8K_REG_GLOBAL_FW_CTRL1 0x624` (qca8k.h:247). Fully identical.

### 1.6 PORT_LOOKUP_CTL — per-port member/state (0x0660 + port*0xc)
Source `isisc_reg.h:1948-2007`; stride `E_OFFSET 0x000c`, `NR_E 7`. qca8k `QCA8K_PORT_LOOKUP_CTRL(_i) (0x660 + (_i)*0xc)` (qca8k.h:252) — identical offset+stride.

| field | bits | meaning | qca8k mapping |
|---|---|---|---|
| MULTI_DROP_EN | [31] | drop multicast | not in qca8k |
| UNI/MUL/ARP_LEAKY_EN | [28]/[27]/[26] | leaky modes | not in qca8k |
| ING_MIRROR_EN | [25] | ingress mirror | not in qca8k |
| PORT_LOOP_BACK | [21] | loopback | not in qca8k |
| LEARN_EN | [20] | SA learning | `QCA8K_PORT_LOOKUP_LEARN BIT(20)` |
| PORT_STATE | [18:16] | STP: 0=disabled,1=blocking,2=listening,3=learning,4+=forwarding | `QCA8K_PORT_LOOKUP_STATE_MASK GENMASK(18,16)` (qca8k.h:259-263) |
| FORCE_PVLAN | [10] | force port VLAN | not exposed in qca8k.h |
| DOT1Q_MODE | [9:8] | 0=none,1=fallback,2=check,3=secure | `QCA8K_PORT_LOOKUP_VLAN_MODE_MASK GENMASK(9,8)` (qca8k.h:254-258) |
| PORT_VID_MEM | [6:0] | port membership bitmap | `QCA8K_PORT_LOOKUP_MEMBER GENMASK(6,0)` (qca8k.h:253) |

Membership and STP-state semantics are byte-for-byte qca8k.

### 1.7 Port numbering / bitmask width
- 7 ports total (0..6), confirmed by every switch-core `NR_E 7` (PORT_STATUS `isisc_reg.h:1016`, PORT_HDR_CTL `:1106`, PORT_LOOKUP_CTL `:1952`) and the 7-bit bitmap fields. Matches `QCA8K_NUM_PORTS 7` (qca8k.h:21).
- Bit index = port number in every bitmap; bit 0 = port 0.
- SPNMX57 usage (`ipq5018-spnmx57.dts`):
  - Port 0 = CPU, forced 2.5G/full, no PHY, SGMII+ to IPQ5018 MAC1 (`dts:56-58,63-64`; `switch_cpu_bmp = <ESS_PORT0>`)
  - Ports 1-4 = user/LAN (QCA8084 EPHYs at MDIO addr 1-4) (`dts:59`, PHY nodes `dts:290-303`, switch port nodes `dts:90-105`)
  - No WAN, `switch_wan_bmp = <0>` (`dts:60`); ports 5,6 unused
- qca8k allows CPU on port 0 or 6 (`QCA8K_CPU_PORT0/QCA8K_CPU_PORT6`, qca8k.h:389-390; `QCA8K_NUM_CPU_PORTS 2` qca8k.h:22). SPNMX57 uses CPU_PORT0 only.

### 1.8 DSA-relevant C `#define` block to copy
```c
/* Chip identity — bind the DSA driver on this */
#define MHT_QCA8386_DEVICE_ID       0x17   /* mht_reg.h:26 / isisc_reg.h:25; MASK_CTRL[15:8] */

/* All offsets/bitfields below are IDENTICAL to drivers/net/dsa/qca/qca8k.h @ v6.18 */
#define QCA8K_REG_MASK_CTRL         0x000     /* DEVICE_ID GENMASK(15,8), REV_ID GENMASK(7,0) */
#define QCA8K_REG_PORT_STATUS(p)    (0x07c + (p)*4)
#define   PORT_STATUS_SPEED         GENMASK(1,0)   /* 0=10 1=100 2=1000; 2500 ALSO uses code 2 */
#define   PORT_STATUS_TXMAC         BIT(2)
#define   PORT_STATUS_RXMAC         BIT(3)
#define   PORT_STATUS_TXFLOW        BIT(4)
#define   PORT_STATUS_RXFLOW        BIT(5)
#define   PORT_STATUS_DUPLEX        BIT(6)
#define   PORT_STATUS_LINK_UP       BIT(8)
#define   PORT_STATUS_LINK_AUTO     BIT(9)   /* clear = force mode (isisc LINK_EN) */
#define   PORT_STATUS_FLOW_AUTO     BIT(12)  /* isisc FLOW_LINK_EN */
#define QCA8K_REG_PORT_HDR_CTRL(p)  (0x09c + (p)*4)   /* RX GENMASK(3,2), TX GENMASK(1,0); NONE0/MGMT1/ALL2 */
#define QCA8K_REG_GLOBAL_FW_CTRL0   0x620    /* CPU_PORT_EN BIT(10) */
#define QCA8K_REG_GLOBAL_FW_CTRL1   0x624    /* IGMP[30:24] BC[22:16] MC[14:8] UC[6:0], 7-bit port bitmaps */
#define QCA8K_PORT_LOOKUP_CTRL(p)   (0x660 + (p)*0xc) /* MEMBER[6:0], VLAN_MODE[9:8], STATE[18:16] */
#define QCA8K_NUM_PORTS             7        /* HW; on SPNMX57 the driver models 5 (see §4/build order) */
```

---

## 2. CPU-uplink MMD / SerDes sequence (uniphy1, MAC_MODE_SGMII_PLUS, forced 2500/full)

Authoritative call path: `mht_interface_mac_mode_set(port0)` -> `mht_interface_sgmii_mode_set(uniphy_index=MHT_UNIPHY_SGMII_1, port0, config)` -> `mht_uniphy_sgmii_function_reset(SGMII_1)`. Board wants `mac_mode=FAL_MAC_MODE_SGMII_PLUS`, `clock_mode=FAL_INTERFACE_CLOCK_MAC_MODE`, `auto_neg=false`, `force_speed=FAL_SPEED_2500` — proven by `_qca_mht_interface_mode_init` at `src/init/ssdk_mht.c:72-112` (SGMII_PLUS `:90-92`, MAC clock `:106`, `auto_neg=!force_en` `:107`, force_speed from `hsl_port_force_speed_get` `:88-90`) and the DTS `switch_mac_mode = <MAC_MODE_SGMII_PLUS>` + `port@0 forced-speed=<2500>` (`ipq5018-spnmx57.dts:53,63-66,116-117`).

### 2.1 Two addressing layers (do not confuse)
**Layer A — the switch register window (32-bit indirect MDIO), used only to discover the serdes MDIO address.** `qca_mht_serdes_addr_get` (`mht_sec_ctrl.c:60-84`) reads `SERDES_CFG_OFFSET` and extracts a 5-bit MDIO address:
- `MHT_UNIPHY_SGMII_0` -> `(data >> SERDES_CFG_S0_ADDR_BOFFSET) & 0x1f` (offset 0)
- `MHT_UNIPHY_SGMII_1` -> `(data >> SERDES_CFG_S1_ADDR_BOFFSET) & 0x1f` (offset 5)  **<- CPU uplink, bits [9:5]**
- `MHT_UNIPHY_XPCS` -> `(data >> SERDES_CFG_S1_XPCS_ADDR_BOFFSET) & 0x1f` (offset 10)

`SERDES_CFG_OFFSET = 0xC90F014`, `S1_XPCS=10, S1=5, S0=0` (`mht_reg.h:165,172,178,184`). This register is itself read through the qca8k-style indirect window: `qca_mht_mii_read`/`_write` = `qca_mii_read`/`qca_mii_write` (`ssdk_plat.h:517-518`, bodies `ssdk_plat.c:372-405`), with `qca_mii_reg_convert` (`ssdk_plat.c:276-289`) ORing `SSDK_SWITCH_REG_TYPE_QCA8386` (`ssdk_plat.h:389`) to tag it as a QCA8386 access. The page-split write is the registered `mii_bus` `sw_read/sw_write` glue (`qca_mii_raw_read/write`, `ssdk_plat.c:291-312`) — this is the kernel qca8k/mdio layer, i.e. the page->pseudo-PHY-0x18-reg-0x0c 32-bit split (the RE'd fact, confirmed in section 4).

**Layer B — the serdes/uniphy is a normal clause-45 MDIO device** at the discovered 5-bit address on the same bus (`&mdio1`, `dts:49,232`). Every write below is `hsl_phy_mmd_reg_write`/`hsl_phy_modify_mmd`/`hsl_phy_modify_mii` with `is_c45 = A_TRUE`:
- MMD accesses use `HSL_PHY_REG_C45_ADDR(mmd,reg) = BIT(30)|mmd<<16|reg` (`hsl_phy.h:872`); `BIT(30)` (`SSDK_ADDR_C45`, `ssdk_plat.h:258`) routes to `__mdiobus_c45_write(bus,phy_addr,devad,reg,val)` (`hsl_phy.c:3042-3051`) — genuine IEEE clause-45 frames.
- MII accesses (regs 0 and 6) are plain clause-22 to the same phy_addr.

**MMD numbers:** `MHT_UNIPHY_MMD1 = 0x1 = MDIO_MMD_PMAPMD` (analog/mode/calibration/reset), `MHT_UNIPHY_MMD3 = 0x3 = MDIO_MMD_PCS` (`mht_interface_ctrl.h:29-30`, kernel `linux/mdio.h:18,21`). Per-channel XPCS MMDs are 31/26/27/28 for channels 0..3 (`mht_interface_ctrl.h:31-34`, `mht_uniphy_xpcs_port_to_mmd` `mht_interface_ctrl.c:94-118`).

**Net for the CPU port at SGMII+ 2500:** address = `qca_mht_serdes_addr_get(MHT_UNIPHY_SGMII_1)` (SERDES_CFG bits [9:5]); MMD device = PMAPMD(1) for every register write, plus clause-22 MII regs 0 and 6. The XPCS address (bits [14:10]) and MMD3/PCS are NOT touched in the SGMII+ path (10G-QXGMII only).

### 2.2 Ordered register writes for SGMII+ forced 2500/full
For this case `mode_ctrl = MHT_UNIPHY_MMD1_SGMII_PLUS_MODE(0x800) | MHT_UNIPHY_MMD1_SGMII_MAC_MODE(0x20) = 0x820`; `raw_clk = UNIPHY_CLK_RATE_312M = 312500000`. Source `mht_interface_sgmii_mode_set`, `mht_interface_ctrl.c:585-740`.

Pre-step (clocks, Layer A / NSSCC — map to Linux `clk` in phylink):

| # | Op | Target | Value | Meaning | Cite |
|---|----|--------|-------|---------|------|
| 0a | clk deassert | `MHT_SRDS1_SYS_CLK` | — | ungate uniphy1 sys clock | `mht_interface_ctrl.c:600-604` |
| 0b | clk assert | `MHT_UNIPHY_XPCS_RST` | — | hold XPCS in reset (SGMII+ unused) | `:655` |
| 0c | raw-clk set | `MHT_P_UNIPHY1_RX`, `MHT_P_UNIPHY1_TX` | 312500000 | uniphy1 rx/tx line = 312.5 MHz (the 2.5x) | `:657-658`; `ssdk_clk.h:304`; also `UQXGMII_SPEED_2500M_CLK` `:143-144`, `ssdk_mht_clk.h:162` |
| 0d | port GMII/uniphy clk | port0 + `MHT_CLK_TYPE_UNIPHY` | disable | quiesce before reconfig | `:636-649` |

Uniphy register sequence (Layer B — clause-45 to the SGMII_1 address; MII = clause-22):

| # | Bus op | MMD / reg | Mask | Value | Meaning | Cite |
|---|--------|-----------|------|-------|---------|------|
| 1 | modify_mii | reg 6 `PLL_LOOP_CONTROL` | 0x30 `CML2CMS_IBSEL` | 0x30 | PLL bias fix, high-temp lock | `:667-670`; `mht_interface_ctrl.h:21,26` |
| 2 | modify_mmd | MMD1 `MODE_CTRL` 0x11b | 0x1f70 `SGMII_MODE_CTRL_MASK` | 0x820 (SGMII_PLUS 0x800 \| MAC 0x20) | SGMII+ mode, uniphy as MAC | `:671-675`; `h:40,49,52,53` |
| 3 | modify_mmd | MMD1 `GMII_DATAPASS_SEL` 0x180 | 0x1 `DATAPASS_MASK` | 0x0 `DATAPASS_SGMII` | route GMII datapath as SGMII (not USXGMII) | `:676-680`; `h:42,60-62` |
| 4 | modify_mmd | MMD1 `CHANNEL0_CFG` 0x120 | 0xe `CH0_FORCE_SPEED_MASK` | 0xc (FORCE_ENABLE 0x8 \| FORCE_SPEED_1G 0x4) | force speed, disable AN. 2500 and 1000 BOTH write the "1G" code 0x4; the 2.5x comes from SGMII_PLUS + 312.5 MHz, not this field | `:682-713` (2500 case `:697-701`); `h:41,54-59` |
| 5 | clk reset | port0 GMII + uniphy clocks | — | reset/release interface clocks | `:714-719` |
| 6 | modify_mii | reg 0 `PLL_POWER_ON_AND_RESET` | 0x40 | 0x00 `ANA_SOFT_RESET` | assert analog soft-reset | `:722-724`; `h:20,24` |
| 7 | delay | — | — | `mdelay(1)` | timings note below | `:725` |
| 8 | modify_mii | reg 0 `PLL_POWER_ON_AND_RESET` | 0x40 | 0x40 `ANA_SOFT_RELEASE` | release analog soft-reset | `:726-728`; `h:25` |
| 9 | poll | MMD1 `CALIBRATION4` 0x78 | 0x80 `CALIBRATION_DONE` | wait set | uniphy calibration done | `:729-731`; `mht_uniphy_calibration` `:205-227` |
| 10 | clk enable | port0 GMII + uniphy clocks | — | re-enable interface clocks | `:732-737` |

SGMII function reset (`mht_uniphy_sgmii_function_reset(SGMII_1)`, called from `mht_interface_mac_mode_set:792`; body `:379-407`):

| # | Bus op | MMD / reg | Mask | Value | Meaning | Cite |
|---|--------|-----------|------|-------|---------|------|
| 11 | modify_mmd | MMD1 `CHANNEL0_CFG` 0x120 | 0x800 `SGMII_ADPT_RESET` | 0 | assert SGMII ch0 adapter reset | `:388-391`; `h:65` |
| 12 | delay+set | MMD1 0x120 | 0x800 | 0x800 | `mdelay(1)` then de-assert | `:392-396` |
| 13 | modify_mmd | MMD1 `USXGMII_RESET` 0x18c | 0x10 `SGMII_FUNC_RESET` | 0 | assert SGMII func/IPG-tune reset | `:397-400`; `h:43,64` |
| 14 | delay+set | MMD1 0x18c | 0x10 | 0x10 | `mdelay(1)` then de-assert | `:401-404` |

That is the complete forced-2500 SGMII+ bring-up. SSCG enable and CDR/SSC-fix are NOT done in the SGMII+ path (UQXGMII-only, `:459-460`; mainline `CDR_CONTRL`/`SSC_FIX_MODE` only in `qca8084_pcs_set_mode`).

### 2.3 Polling / reset timings
- Analog soft-reset pulse (6-8): assert 0, `mdelay(1)` (`:725`), release. UQXGMII path uses `mdelay(10)` (`:449`); mainline `qca8084_do_calibration` uses `usleep_range(10000,11000)`. **10 ms is the safer value to port**; SGMII path currently uses 1 ms.
- Calibration poll (9): up to `retries=100`, `mdelay(1)` each -> ~100 ms, poll MMD1 0x78 bit 0x80 (`:209-224`). Mainline: `read_poll_timeout(...,100,100000,...)` = 100 us interval, 100 ms timeout.
- SGMII adapter+function reset pulses (11-14): assert -> `mdelay(1)` -> de-assert (`:392,:401`). Mainline `qca8084_pcs_ipg_tune_reset` uses `usleep_range(1000,1100)`.
- XPCS reset is asserted and left asserted for SGMII+ (`ssdk_mht_clk_assert(MHT_UNIPHY_XPCS_RST)`, `:655`); no XPCS poll in this path.
- No 10G-BaseR link poll / no XPCS soft-reset poll in SGMII+ (UQXGMII-only: `:229-251`, `:253-276`).

For phylink, the reset/calibration polls (6-9, 11-14) live in `pcs_config`/`mac_prepare`; the 312.5 MHz rate (step 0c) is a `clk_set_rate` on uniphy1 rx/tx clocks.

### 2.4 Relationship: package XPCS/PCS serdes vs the switch CPU-port uplink serdes
Same silicon IP (Qualcomm "uniphy"/Napa serdes + XPCS/PCS), same register map, reached and clocked differently. Mainline `qca8084_serdes.c` (`0940-*.patch`) and the SSDK MHT uniphy code define the same registers at the same MMDs with the same encodings:

| Meaning | SSDK (`mht_interface_ctrl.h`) | Mainline (`0940-*.patch`) | Same? |
|---|---|---|---|
| Mode ctrl reg | MMD1 MODE_CTRL 0x11b | PMAPMD MODE_CONTROL 0x11b | yes (PMAPMD=MMD1) |
| SGMII+ select | SGMII_PLUS_MODE 0x800 | `FIELD_PREP(GENMASK(12,8), 0x8)` = 0x800 | yes |
| SGMII select | SGMII_MODE 0x400 | SGMII=0x4 -> 0x400 | yes |
| XPCS select | XPCS_MODE 0x1000 | XPCS=0x10 -> 0x1000 | yes |
| MAC / PHY clk | MAC 0x20 / PHY 0x10 | SGMII_MAC 2 / SGMII_PHY 1 in GENMASK(6,4) -> 0x20 / 0x10 | yes |
| Datapass | GMII_DATAPASS_SEL 0x180, SGMII=0 | QP_USXG_OPTION1 0x180, DATAPASS bit0=0 | yes |
| Calibration | MMD1 CALIBRATION4 0x78, done 0x80 | PMAPMD CALIBRATION4 0x78, `CALIBRATION_DONE BIT(7)` | yes |
| Ana reset | MII reg0 bit 0x40 | `PLL_POWER_ON_AND_RESET 0x0`, `PCS_ANA_SW_RESET BIT(6)` | yes |
| PLL bias | MII reg6 mask 0x30 | `PLL_CONTROL 6`, `CMLDIV2_IBSEL GENMASK(5,4)` | yes |
| Func reset | USXGMII_RESET 0x18c, SGMII_FUNC_RESET 0x10 | QP_USXG_RESET 0x18c, `QP_USXG_SGMII_FUNC_RESET BIT(4)` | yes |
| Per-channel MMDs | 31/26/27/28 | `qca8084_xpcs_ch_mmd[] = {31,26,27,28}` | yes |
| 2500 raw clock | 312500000 | SPEED_2500 -> rate 312500000 | yes |

Mainline `qca8084_pcs_set_interface_mode` already handles `PHY_INTERFACE_MODE_2500BASEX -> MODE_CONTROL_SGMII_PLUS` with `DATAPASS_SGMII` (exactly this case), even though the merged series only calls it with `PHY_INTERFACE_MODE_10G_QXGMII` (from `qca8084_pcs_set_mode`).

Differences: (1) role/instances — the QCA8386 die has SGMII_0 (external port5), SGMII_1 (port0 CPU uplink), and the shared XPCS (fans uniphy1 to the 4 EPHYs in QXGMII); (2) addressing — switch reads the address at runtime from SERDES_CFG, the standalone package fixes it in DT `pcs-phy`/`xpcs-phy`; (3) clock/reset — switch uses `ssdk_mht_clk_*` via the internal GCC, package uses the Linux `clk`/`reset` framework against the nsscc; (4) mode on SPNMX57 — CPU uplink runs plain 1-lane SGMII+ 2500 (MMD1/PMAPMD only, XPCS held in reset), whereas the mainline package as-shipped only wires 10G-QXGMII.

**Porting implication:** the writes in 2.2 are exactly the `qca8084_pcs_set_interface_mode(PHY_INTERFACE_MODE_2500BASEX)` branch (SGMII_PLUS + DATAPASS_SGMII) plus PLL bias, analog reset, calibration poll, and the SGMII adapter/function resets, all on MMD1/PMAPMD of the SGMII_1 address at 312.5 MHz. You do NOT need MMD3/PCS BaseR, USXGMII_EN, QXGMII_EN, AM-interval, XPCS soft-reset, or per-channel MMDs. In a qca8k-style DSA driver this belongs in the CPU port `phylink_pcs` `pcs_config`/`pcs_link_up` (or DSA `port_config` for a fixed-link CPU port), with the serdes reached by resolving SERDES_CFG[9:5] to a clause-45 `mdio_device` on the switch's MDIO bus.

---

## 3. nsscc + qca8084-package device tree

All nodes go under `&mdio1` (old-stack `dts:49` `mdio-bus = <&mdio1>`, `:232` `&mdio1 { status = "okay" }`). Values that need a live MDIO scan are tagged [VALIDATE].

Add to the board .dts:
```
#include <dt-bindings/clock/qcom,qca8k-nsscc.h>
#include <dt-bindings/reset/qcom,qca8k-nsscc.h>
#include <dt-bindings/net/qcom,qca808x.h>
```
(Same set the binding example uses; the driver includes both nsscc headers at `/tmp/nsscc-qca8k.c:16-17`.)

### 3.1 NSS clock/reset controller node (`nsscc-qca8k`)
```dts
&mdio1 {
    #address-cells = <1>;
    #size-cells = <0>;

    qca8k_nsscc: clock-controller@18 {
        compatible = "qcom,qca8386-nsscc", "qcom,qca8084-nsscc";
        reg = <0x18>;

        /* THIS node, and only this node, owns the gpio24 package reset. */
        reset-gpios = <&tlmm 24 GPIO_ACTIVE_LOW>;

        /* 7 parents, POSITIONAL (no clock-names), order = DT_XO..DT_UNIPHY1_TX312P5M_CLK */
        clocks = <&qca8386_ref50m>,         /* DT_XO : 50 MHz chip ref  [VALIDATE] */
                 <&qca8k_uniphy0_rx>,       /* DT_UNIPHY0_RX_CLK   [NO in-tree provider] */
                 <&qca8k_uniphy0_tx>,       /* DT_UNIPHY0_TX_CLK   [NO in-tree provider] */
                 <&qca8k_uniphy1_rx>,       /* DT_UNIPHY1_RX_CLK   [NO in-tree provider] */
                 <&qca8k_uniphy1_tx>,       /* DT_UNIPHY1_TX_CLK   [NO in-tree provider] */
                 <&qca8k_uniphy1_rx312p5m>, /* DT_UNIPHY1_RX312P5M_CLK [NO in-tree provider] */
                 <&qca8k_uniphy1_tx312p5m>; /* DT_UNIPHY1_TX312P5M_CLK [NO in-tree provider] */

        #clock-cells = <1>;
        #reset-cells = <1>;
        #power-domain-cells = <1>;
    };
};
```

- **compatible** — driver matches only `qcom,qca8084-nsscc` (`/tmp/nsscc-qca8k.c:2204-2206`). The two-item `qcom,qca8386-nsscc`,`qcom,qca8084-nsscc` is the correct precise value (binding `qcom,qca8k-nsscc.yaml` @v6.18 lists `qca8386-nsscc` as a prefix enum with `const: qcom,qca8084-nsscc`). Bare `qcom,qca8084-nsscc` also works.
- **reg = <0x18>** — the driver is an `mdio_driver` (`:2208`); it ignores `mdiodev->addr` and hardcodes the window: page to phy 0x18 reg 0x0c (`:28` `QCA8K_HIGH_ADDR_PREFIX 0x18`, `:30` `QCA8K_CFG_PAGE_REG 0xc`, `:2084` `qca8k_mii_page_set(bus, QCA8K_HIGH_ADDR_PREFIX, QCA8K_CFG_PAGE_REG, page)`), data at phy `0x10 | regbits[7:5]` (`:29` `QCA8K_LOW_ADDR_PREFIX 0x10`, `:2027`). 0x18 matches the binding example and sits inside the reserved 0x10-0x1f block. [VALIDATE] nothing else answers at 0x18 (any 0x10-0x1f is functionally equivalent since the window is hardcoded).
- **reset-gpios = <&tlmm 24 GPIO_ACTIVE_LOW>** — GPIO24 active-low, from old-stack `dts:272` `qca8084-reset-gpios = <&tlmm 24 GPIO_ACTIVE_LOW>` (vendor `<&tlmm 0x18 0x00>` = gpio24, low-then-high pulse). The nsscc claims property `reset` (`:2177` `devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH)`) and pulses assert(100 ms)/deassert (`:2170-2183`). [VALIDATE] polarity: if the package never leaves reset try `GPIO_ACTIVE_HIGH`.
- **clocks (7, positional, NO clock-names)** — binding `required: [compatible, clocks, reg, reset-gpios]`; driver consumes by `.index` (`:36-44` enum, `:60-63`). [VALIDATE — hard gap] there is NO in-tree provider for the six `qca8k_uniphy*` clocks in v6.18 (the serdes patches `0939/0940/0941` only add PCS/XPCS probe, register no `qca8k_uniphy*` clock; no v6.18 board .dts instantiates `qca8084-nsscc`; no qca8k PCS clock-provider in `drivers/net/pcs/`). To make the probe run you must supply `fixed-clock` stubs (UNIPHY1 tx/rx312p5m = 312500000, the /125M lines = 125000000) or add a PCS clock-provider, and confirm link clocking on HW. `&qca8386_ref50m` (DT_XO) likewise needs a real 50 MHz source; model as `fixed-clock` and confirm against the schematic.
- **#clock-cells/#reset-cells/#power-domain-cells = <1>** — from the binding example; driver registers via `qcom_cc_really_probe` (`:2201`).

### 3.2 QCA8084 PHY-package node
```dts
&mdio1 {
    ethernet-phy-package@1 {
        #address-cells = <1>;
        #size-cells = <0>;
        compatible = "qcom,qca8084-package";
        reg = <1>;

        clocks = <&qca8k_nsscc NSS_CC_APB_BRIDGE_CLK>,
                 <&qca8k_nsscc NSS_CC_AHB_CLK>,
                 <&qca8k_nsscc NSS_CC_SEC_CTRL_AHB_CLK>,
                 <&qca8k_nsscc NSS_CC_TLMM_CLK>,
                 <&qca8k_nsscc NSS_CC_TLMM_AHB_CLK>,
                 <&qca8k_nsscc NSS_CC_CNOC_AHB_CLK>,
                 <&qca8k_nsscc NSS_CC_MDIO_AHB_CLK>;
        clock-names = "apb_bridge", "ahb", "sec_ctrl_ahb",
                      "tlmm", "tlmm_ahb", "cnoc_ahb", "mdio_ahb";

        resets = <&qca8k_nsscc NSS_CC_GEPHY_FULL_ARES>;

        /* SPNMX57 uplink is SGMII+ 2.5G (switch mode). Board evidence => set 1, NOT the default 0. */
        qcom,package-mode = <QCA808X_PCS1_SGMII_MAC_PCS0_SGMII_MAC>;   /* =1  [VALIDATE] */
        qcom,phy-addr-fixup = <1 2 3 4 5 6 7>;

        ethernet-phy@1 { compatible="ethernet-phy-id004d.d180"; reg=<1>;
            clocks=<&qca8k_nsscc NSS_CC_GEPHY0_SYS_CLK>; resets=<&qca8k_nsscc NSS_CC_GEPHY0_SYS_ARES>;
            qcom,xpcs-channel=<0>; };   /* xpcs-channel meaningful only in QXGMII */
        ethernet-phy@2 { compatible="ethernet-phy-id004d.d180"; reg=<2>;
            clocks=<&qca8k_nsscc NSS_CC_GEPHY1_SYS_CLK>; resets=<&qca8k_nsscc NSS_CC_GEPHY1_SYS_ARES>;
            qcom,xpcs-channel=<1>; };
        ethernet-phy@3 { compatible="ethernet-phy-id004d.d180"; reg=<3>;
            clocks=<&qca8k_nsscc NSS_CC_GEPHY2_SYS_CLK>; resets=<&qca8k_nsscc NSS_CC_GEPHY2_SYS_ARES>;
            qcom,xpcs-channel=<2>; };
        ethernet-phy@4 { compatible="ethernet-phy-id004d.d180"; reg=<4>;
            clocks=<&qca8k_nsscc NSS_CC_GEPHY3_SYS_CLK>; resets=<&qca8k_nsscc NSS_CC_GEPHY3_SYS_ARES>;
            qcom,xpcs-channel=<3>; };
        /* pcs-phy@6 / xpcs-phy@7 : 10G-QXGMII ONLY; omit in SGMII+ switch mode. */
    };
};
```

- **compatible = "qcom,qca8084-package"** — `0930-*.patch:78`, example `:319-320`.
- **reg = <1>** — package base = first EPHY. `0930-*.patch:322`. SPNMX57 EPHYs answer at MDIO 1..4 (`dts:290-303`, live ID 0x004dd180), so base 1 is correct.
- **clocks + clock-names (7)** — list/names from `0930-*.patch:88-104` (`:96`) + example `:323-336`. IDs from `qcom,qca8k-nsscc.h`: `NSS_CC_APB_BRIDGE_CLK=2`, `NSS_CC_AHB_CLK=78`, `NSS_CC_SEC_CTRL_AHB_CLK=79`, `NSS_CC_TLMM_CLK=80`, `NSS_CC_TLMM_AHB_CLK=81`, `NSS_CC_CNOC_AHB_CLK=82`, `NSS_CC_MDIO_AHB_CLK=83`. Driver reads by name (`0936-*.patch` `qca8084_package_clk_name[]`); optional `mdio_master_ahb`/`switch_core` treated as `-EINVAL`->skip, so omitting is fine.
- **resets = <&qca8k_nsscc NSS_CC_GEPHY_FULL_ARES>** — single reset (`0930-*.patch:105-111`, example `:337`), `NSS_CC_GEPHY_FULL_ARES = 65`. This is the package DSP reset over MDIO, NOT the GPIO. Deasserted at `0936-*.patch`.
- **qcom,phy-addr-fixup = <1 2 3 4 5 6 7>** — [PHY0..3, PCS0, PCS1-PCS, PCS1-XPCS] (`0930-*.patch:133-140`, example `:339`). First four confirmed for SPNMX57. [VALIDATE] last three (5/6/7) with a live scan (this is a different register path from qca-ssdk EPHY_CFG).
- **qcom,package-mode** — `0930-*.patch:112-131`, enum [0,1,2], default 0. Values from `qcom,qca808x.h`: `QCA808X_PCS1_10G_QXGMII_PCS0_UNUNSED=0`, `QCA808X_PCS1_SGMII_MAC_PCS0_SGMII_MAC=1`, `QCA808X_PCS1_SGMII_MAC_PCS0_SGMII_PHY=2`. SPNMX57 is SGMII+ 2.5G switch mode (`dts:53/117` SGMII_PLUS, CPU forced-speed 2500 `:66/172`) — corroborated by section 2 (uniphy1 SGMII+, XPCS held in reset). Use **1**, not 0. [VALIDATE] on HW.
- **qcom,xpcs-channel** — `0930-*.patch:141-149`, IDs 0/1/2/3; per-child clocks/resets `NSS_CC_GEPHY{0..3}_SYS_CLK` (88/89/90/91) and `_SYS_ARES` (52/53/54/55). [VALIDATE] inert when `package-mode=1`; keep only for QXGMII.
- **Child compatible = "ethernet-phy-id004d.d180"** — matches live PHY ID 0x004dd180 (`dts:288`, binding `0930-*.patch:150`).

### 3.3 gpio24 reset ownership — UNAMBIGUOUS within the DT
The `clock-controller@18` (nsscc) owns gpio24 and is the ONLY node that may carry `reset-gpios`:
- nsscc claims+pulses it before it can talk MDIO for the package clocks (`/tmp/nsscc-qca8k.c:2170-2183`, then `:2188-2201`); binding makes `reset-gpios` required on this node.
- `ethernet-phy-package@1` must NOT have `reset-gpios`; it resets via `resets = <&qca8k_nsscc NSS_CC_GEPHY_FULL_ARES>` (`0930-*.patch:105-111`, `0936-*.patch`). A GPIO here would be a second claimant.
- Delete the old-stack `qca8084-reset-gpios` on `&mdio1` (`dts:272`), plus `qcom,qca8084-preinit*`, `reset-delay-us`, `reset-post-delay-us` (`:250-274`) and bare `ethernet-phy@1..4` stubs (`:290-303`) — the package node replaces them.
- Do NOT put the standard MDIO `reset-gpios` on the `&mdio1` bus node either (`__mdiobus_register()` would pulse it independently, a third claimant).

**Cross-section note:** section 4's fork map (§B) says the DSA `qca8k_sw_probe` should keep its own reset-gpios block driving gpio24. That collides with nsscc ownership. See cross-check C-1 for the resolution (the DSA node must NOT claim gpio24).

### 3.4 DT source list
- Package binding + example + `qcom,qca808x.h`: `.../patches-6.18/0930-dt-bindings-net-Document-Qualcomm-QCA8084-PHY-packag.patch`
- Package clocks/resets consumer: `.../0936-net-phy-qca808x-Add-package-clocks-and-resets-for-QC.patch`
- PCS/XPCS probe wiring: `.../0939-net-phy-qca808x-Add-QCA8084-SerDes-probe-and-remove-.patch`
- nsscc driver: `srv-openstack:/tmp/nsscc-qca8k.c` (mirror of `drivers/clk/qcom/nsscc-qca8k.c` @v6.18)
- nsscc binding: `Documentation/devicetree/bindings/clock/qcom,qca8k-nsscc.yaml`
- Clock IDs / Reset IDs: `.../include/dt-bindings/clock/qcom,qca8k-nsscc.h` and `.../reset/qcom,qca8k-nsscc.h`
- Board topology/GPIO/mdio: `ipq5018-spnmx57.dts`

---

## 4. qca8k -> `qca8386.c` fork map (from mainline v6.18)

Line numbers are from the raw v6.18 files: `qca8k-8xxx.c` (2228 lines, MDIO-attached body), `qca8k-common.c` (1258 lines, switch-core ops), `qca8k.h` (596 lines).

Grounding for the deltas: page write goes to pseudo-PHY 0x18 (`isisc_reg_access.c:63-64`), and the QCA8386 page register is 0x0c (Phase-1 RE, routed through the external mdio `sw_read/sw_write` at `ssdk_plat.c:333-357`). CPU uplink is 2.5G SGMII+/UQXGMII (`shell_io.c:1691-1693` `PORT_SGMII_PLUS`; `UQXGMII_SPEED_2500M_CLK` throughout `ssdk_mht_clk.c`). Board authoritative: `dts:45-66, 90-108, 277-312, 252-272`.

### 4.A Register-access layer (`qca8k-8xxx.c`)

| Symbol | Lines | Verdict | Change |
|---|---|---|---|
| `qca8k_split_addr` | 27-38 | COPY VERBATIM | same addressing math (word=reg>>1, r1=&0x1e, r2=(>>5)&0x7, page=(>>8)&0x3ff), confirmed identical `isisc_reg_access.c:60-92` |
| `qca8k_mii_write_lo/hi`, `qca8k_mii_read_lo/hi`, `qca8k_mii_read32`, `qca8k_mii_write32` | 40-139 | COPY VERBATIM | chip-independent 16/32-bit MDIO |
| `qca8k_set_page` | 141-161 | MODIFY (one line) | line 151 `bus->write(bus, 0x18, 0, page)` -> `bus->write(bus, 0x18, QCA8386_MDIO_PAGE_REG, page)` with `#define QCA8386_MDIO_PAGE_REG 0x0c`. Keep the page==cached fast-path (148-149), error log, `usleep_range(1000,2000)` (159). THE defining register-access delta. |
| `qca8k_read_mii` / `qca8k_write_mii` | 428-470 | COPY VERBATIM | data window `0x10 | r2` confirmed identical (`isisc_reg_access.c:84,143`); transitively picks up the modified set_page |
| `qca8k_regmap_update_bits_mii` | 472-501 | COPY VERBATIM | RMW over same window |
| `qca8k_bulk_read` / `qca8k_bulk_gather_write` / `qca8k_bulk_write` | 503-555 | COPY VERBATIM | falls back to per-reg mii when no mgmt conduit; eth branch stays dormant |
| `qca8k_regmap_update_bits` | 557-566 | COPY VERBATIM | tries eth, falls back to _mii |
| `qca8k_regmap_config` | 568-584 | MODIFY (fields) | keep reg_bits=16, val_bits=32, reg_stride=4, read/write/reg_update_bits=bulk*, disable_locking=true, cache_type=NONE, max_raw_read=32, use_single_write=true (ATU-bug workaround). CHANGE `.max_register` (0x16ac is the QCA8337 end-MIB bound) and `.rd_table` (QCA8337 ranges, `common.c:81-102`) to the QCA8386 map. Instantiated bus-less at `sw_probe:2079-2080` `devm_regmap_init(&mdiodev->dev, NULL, priv, &qca8k_regmap_config)` — keep call shape. |

**DROP the entire MDIO-MASTER / eth-mgmt PHY machinery** (external QCA8084s -> no internal-PHY access through the switch): `qca8k_phy_eth_command` (622-791), `qca8k_mdio_busy_wait` (793-813), `qca8k_mdio_write/read` (815-895), `qca8k_internal_mdio_write/read` (897-928), `qca8k_legacy_mdio_write/read` (930-944), `qca8k_mdio_register` (946-992), `qca8k_setup_mdio_bus` (994-1061). The eth rw/ack/mib-autocast helpers (163-620, 1644-1727, 1752-1790) can be dropped for a lean bring-up (the SPNMX57 CPU link is a plain fixed 2.5G conduit, not a tag-mgmt conduit).

### 4.B Probe / ID / driver registration
- **`qca8k_read_switch_id`** — `common.c:1230-1257`. MODIFY (match value only). Reads MASK_CTRL 0x000, id bits 15:8, rev 7:0; compare `id != priv->info->id` (1244). Add `#define QCA8386_ID 0x17` and match_data `.id = 0x17`. Body verbatim. Verify on HW that 0x17 reads at MASK_CTRL[15:8].
- **`qca8k_sw_probe`** — `qca8k-8xxx.c:2048-2112`. MODIFY. Keep priv alloc (2057), `priv->bus = mdiodev->bus` (2061), `priv->info` (2063), regmap init (2079-2084) at the modified config, `priv->mdio_cache.page = 0xffff` (2086), `qca8k_read_switch_id` (2089), ds alloc + `ds->priv/ops/phylink_mac_ops` (2093-2107), reg_mutex init, `dsa_register_switch` (2111). CHANGE `num_ports = QCA8K_NUM_PORTS` (2104) -> 5 (CPU 0 + user 1..4); point ds->ops at `qca8386_switch_ops` and ds->phylink_mac_ops at `qca8386_phylink_mac_ops`. **CONFLICT (see C-1): the reset_gpio block (2065-2076) that drives DT `reset-gpios` on gpio24 must be DROPPED/skipped for the fork** — the nsscc owns gpio24; a second claim fails with -EBUSY. Keep mgmt/mib mutex+completion inits (2097-2101) only if the eth path is kept.
- **mdio_driver registration** — `qca8k-8xxx.c`: `qca8k_of_match` (2203-2209), `qca8kmdio_driver` (2211-2220), `mdio_module_driver` (2222), `qca8k_match_data` table (2184-2201), `qca8xxx_ops` (2180-2182). MODIFY: single `{ .compatible = "qca,qca8386", .data = &qca8386 }`; `static const struct qca8k_match_data qca8386 = { .id = QCA8386_ID, .mib_count = <n>, .ops = ... }`; `.mdiodrv.driver.name = "qca8386"`. Drop `autocast_mib` if the eth path is dropped (then guard `common.c:502-503`). Keep `SIMPLE_DEV_PM_OPS`/suspend/resume (2141-2178) and `qca8k_sw_remove`/`qca8k_sw_shutdown` (2114-2139) verbatim (loop bound 2123 follows reduced port count).

### 4.C `qca8k_setup` — `qca8k-8xxx.c:1832-2002`
Keep the structure; every register offset it writes is QCA8337-family (`qca8k.h`) and must be confirmed against the QCA8386 map (see Unresolved).
- `qca8k_find_cpu_port` (1840; def 1090-1104) — MODIFY/simplify: only port 0 (`dts:58`), port-6 fallback (1100-1101) is dead.
- `qca8k_parse_port_config` (1847; def 1146-1242) — DROP or stub: parses RGMII/SGMII delays for CPU ports 0/6; QCA8386 uplink is fixed 2.5G SGMII+ with no RGMII pad delays.
- `qca8k_setup_mdio_bus` (1851; def 994-1061) — DROP entirely: EPHYs are external QCA8084s on `mdio1`, bound by the mainline package driver (patches 0930..0941) and referenced from user ports via `phy-handle`. Do NOT enable `QCA8K_MDIO_MASTER_EN`. If the QCA8386 has an MDC-passthrough bit, clear it once (verify).
- `qca8k_setup_of_pws_reg` (1855; def 1106-1144) — DROP: QCA8327/8337 gated on `switch_id == QCA8K_ID_QCA8327`; never matches 0x17.
- `qca8k_setup_mac_pwr_sel` (1859; def 1063-1088) — DROP: ipq8064/8065 only.
- `qca8k_setup_led_ctrl` (1863; `qca8k_leds.c`) — DROP for bring-up; QCA8386 LED map differs.
- `qca8k_setup_pcs` (1867-1868; def 1633-1642) — MODIFY: keep container/ops but only CPU-side `pcs_port_0`; drop `pcs_port_6`.
- MAC06-exchange disable (1871-1876) — DROP: `QCA8K_REG_PORT0_PAD_CTRL` bit31 is QCA8337-specific.
- Enable-CPU-port (1879-1884, FORWARD_CTL0 CPU_PORT_EN §1.4), `qca8k_mib_init` (1887), disable-forwarding/MAC loops (1892-1902), QCA-header-mode on CPU ports (1905-1913, PORT_HDR_CTL §1.3), unknown-frame flood to CPU (1919-1925, FORWARD_CTL1 §1.5), CPU<->user port-lookup membership (1928-1964, PORT_LOOKUP_CTL §1.6), MAX_FRAME_SIZE (1987), `qca8k_fdb_flush` (1992), ageing min/max (1995-1996), `num_lag_ids` (1999) — KEEP logic; each offset must be confirmed for QCA8386. Iterators auto-scale with num_ports=5.
- HOL fixup (1966-1974; def 1792-1830) — DROP: gated on `QCA8K_ID_QCA8337`.
- GLOBAL_FC_THRESH (1977-1984) — DROP: gated on `QCA8K_ID_QCA8327`.

### 4.D phylink
- **`qca8k_phylink_get_caps`** — 1400-1433. MODIFY: port 0/CPU (1404-1408) replace RGMII+SGMII with the 2.5G set (`__set_bit(PHY_INTERFACE_MODE_2500BASEX, ...)`, optionally SGMII), drop RGMII. Ports 1-5 "internal PHY" branch (1410-1420) is wrong (user ports face external QCA8084s) — set the actual MAC<->QCA8084 interface (SGMII / 2500BASEX; UQXGMII in vendor terms), remove port 5. `mac_capabilities` (1431-1432) `... | MAC_1000FD` -> **add `MAC_2500FD`**.
- **`qca8k_phylink_mac_select_pcs`** — 1285-1313. MODIFY: keep `container_of`; port 0 -> `pcs_port_0`; drop port-6; add per-user-port PCS if the QCA8084 links run 2500BASEX and need switch-side PCS.
- **`qca8k_phylink_mac_config`** — 1315-1398. MODIFY (largely rewrite): the port dispatch (1328-1362) and RGMII/SGMII pad programming write `QCA8K_REG_PORT0_PAD_CTRL`/`PORT6_PAD_CTRL` (QCA8337 pad regs). For QCA8386 this becomes the MAC/uniphy SGMII+/2500 bring-up on the CPU port (the section-2 sequence; SSDK `mht_interface_ctrl.c`). Keep the "internal PHY, nothing to do" early-return for user ports only if the QCA8084 side is fully PCS-driven; else add user-port MAC config. Drop port-6 (1347-1358).
- **`qca8k_phylink_mac_link_up`** — 1445-1487. MODIFY. Speed switch (1459-1472) encodes only 10/100/1000 via `QCA8K_PORT_STATUS_SPEED_10/100/1000`; add `case SPEED_2500:`. **Per §1.2 write the 1000 code (value 2), NOT a new/wider field** (see C-3). Once speed is encoded, keep the existing DUPLEX_FULL/RXFLOW/TXFLOW/TXMAC|RXMAC logic (1474-1486) verbatim; CPU port is fixed 2500/full (`dts:64-66`) with `phylink_autoneg_inband(mode)` false.
- `qca8k_phylink_mac_link_down` — 1435-1443. COPY VERBATIM.
- PCS ops: `qca8k_pcs_get_state` (1494-1532) MODIFY — for the CPU port, report 2500 from the serdes/PCS, not from PORT_STATUS (the switch get-path can't distinguish 2500, §1.2). `qca8k_pcs_config` (1534-1621) MODIFY — the `QCA8K_REG_SGMII_CTRL` programming (1569-1591) is QCA8337-specific; replace with the QCA8386 SGMII+/2500 serdes config (section 2). `qca8k_pcs_an_restart` (1623-1625) verbatim. `qca8k_pcs_ops`/`qca8k_setup_pcs` (1627-1642) keep shape.
- **`qca8k_phylink_mac_ops`** — 2004-2009. Template for `qca8386_phylink_mac_ops`.

### 4.E `qca8k-common.c` switch-core ops
Reusable UNCHANGED with two caveats: (1) offsets/masks come from `qca8k.h` (QCA8337-family) and must be confirmed for the QCA8386 core; (2) loops bounded by `QCA8K_NUM_PORTS` (7) must follow the reduced port count — cleanest is to redefine it to 5 (GENMASK port masks tolerate unused high bits zero).

COPY VERBATIM (pure logic): `qca8k_read/write/rmw` 66-79; `qca8k_busy_wait` 104-110; FDB/ATU 112-323; VTU/VLAN 325-437; ethtool `qca8k_get_strings`/`_ethtool_stats`/`_sset_count` 480-534; learning/STP `qca8k_port_configure_learning` 560-573, `qca8k_port_stp_state_set` 575-608; bridge/port-lookup `qca8k_update_port_member` 610-654, `qca8k_port_pre_bridge_flags`/`_bridge_flags`/`_bridge_join`/`_bridge_leave` 656-718; `qca8k_port_fast_age`/`_set_ageing_time` 720-747; `qca8k_port_enable`/`_disable` 749-769; FDB DSA ops 814-869; MDB 871-898; mirror 900-989; VLAN DSA ops 991-1052; LAG 1054-1228; `qca8k_mib_init` 439-464 and `qca8k_set_mac_eee` 536-558 (contingent on MIB/EEE offsets); `ar8327_mib[]` 22-64 (verify `QCA8K_PORT_MIB_COUNTER` base 0x1000/stride 0x100, qca8k.h:338).

MODIFY: `qca8k_readable_ranges`/`_table` 81-102 (rebuild for the QCA8386 map; this is the regmap `.rd_table`); `qca8k_port_set_status` 466-478 (the `port>0 && port<6` OR-in of `LINK_AUTO` at 471-472 assumes internal PHYs — user ports 1..4 face external QCA8084s fed by phylink; CPU-port list {0,6}->{0}); `qca8k_port_change_mtu` 771-807 (hardcodes CPU ports 0 AND 6 for the off/on dance 790-804; reduce to port 0; `qca8k_port_max_mtu` 809-812 verbatim); `qca8k_read_switch_id` 1230-1257 (match 0x17, §4.B).

**`qca8k_switch_ops`** — `qca8k-8xxx.c:2011-2046`. Template for `qca8386_switch_ops`: FDB/VLAN/bridge/mirror/LAG/MTU/STP/ethtool/EEE entries carry over verbatim. Drop `conduit_state_change`/`connect_tag_protocol` (2044-2045) and `get_phy_flags` (1729-1743, QCA8337 rev to internal PHY, meaningless for external QCA8084) if the eth/internal-PHY paths are dropped. Keep `get_tag_protocol` (1745-1750) only if the CPU link uses `DSA_TAG_PROTO_QCA` tagging — on a plain fixed 2.5G conduit make a real tagging decision (verify).

### 4.F Header (`qca8k.h`) changes
- `QCA8K_NUM_PORTS` 7 -> 5 (line 21); `QCA8K_NUM_CPU_PORTS` 2 -> 1 (line 22).
- Add `#define QCA8386_ID 0x17` (near 27-30).
- Add `#define QCA8386_MDIO_PAGE_REG 0x0c` (used by modified `set_page`).
- **Do NOT add a distinct `QCA8K_PORT_STATUS_SPEED_2500` code and do NOT widen `QCA8K_PORT_STATUS_SPEED` (138-141).** Per §1.2, `SPEED_2500` maps to the existing 1000 code; the `mac_link_up` `case SPEED_2500:` writes `QCA8K_PORT_STATUS_SPEED_1000`. (This overrides section 4's original "widen/verify" note — see C-3.)
- `QCA8K_CPU_PORT0/PORT6` enum (388-391) and `ports_config` CPU arrays (413-414) shrink to one CPU port.
- `struct qca8k_priv` (444-471): keep; drop `pcs_port_6` (468) and internal/eth-mgmt members if those paths are dropped. Keep `mdio_cache.page` (417-423, 466).
- `qca8k_port_to_phy` (486-498) — internal-PHY mapping, unused once MDIO-master is dropped; remove.

---

## Cross-checks: consistency and conflicts (read before writing code)

The four sections mostly agree. The findings below are the points where they interact — three genuine conflicts to resolve in code, plus the confirmations.

**C-1 (CONFLICT — gpio24 reset ownership). Sections 3 and 4 disagree on who claims the package reset.** Section 3 (§3.3) is unambiguous from the driver: the nsscc `clock-controller@18` claims `reset-gpios` (`/tmp/nsscc-qca8k.c:2177`) and pulses gpio24 before it can bring up the package clocks over MDIO; the binding makes `reset-gpios` required on that node; no other node may claim gpio24. Section 4 (§4.B) says keep `qca8k_sw_probe`'s reset_gpio block (2065-2076), which drives the same DT `reset-gpios` on gpio24. **Resolution:** in `qca8386.c`, DROP (or leave unpopulated) the reset-gpios claim in `sw_probe`; the switch DT node must NOT carry `reset-gpios`. gpio24 belongs to the nsscc. Two `devm_gpiod_get*` claimants on one GPIO -> the second gets -EBUSY, and the switch would race the package reset. [VALIDATE on HW that only the nsscc pulses gpio24 and the switch comes up after the package is out of reset.]

**C-2 (CONFLICT — CPU-uplink serdes ownership). Sections 2 and 3 imply two possible owners of uniphy1/SGMII_1 bring-up.** Section 2 puts the SGMII+ 2500 sequence in the DSA driver's CPU-port `pcs_config`/`pcs_link_up` (resolve SERDES_CFG[9:5] -> clause-45 mdio_device). Section 3's package node has `qcom,package-mode` consumed by the qca8084-package PHY driver, which in switch mode (=1) configures PCS1 (=uniphy1) as an SGMII MAC. If both drivers touch uniphy1, they fight. **Resolution (design decision, validate):** on SPNMX57 the CPU uplink is a switch link, so the DSA driver's PCS should own uniphy1/SGMII_1 (section 2), and the package driver should own only the four EPHYs + nsscc-provided clocks/resets. Confirm `package-mode=1` does not also drive uniphy1 into a conflicting state; if it does, the boundary needs an explicit split (or the package configures uniphy1 and the DSA driver only forces PORT_STATUS). This coexistence (a DSA switch driver + the package PHY driver + nsscc on one chip) has no mainline precedent and must be validated end-to-end.

**C-3 (CONFLICT — 2500 PORT_STATUS encoding). Sections 1 and 4 disagree on the field.** Section 4 (§4.D/§4.F) says "add a `QCA8K_PORT_STATUS_SPEED_2500` encoding" and "widen/verify `QCA8K_PORT_STATUS_SPEED`". Section 1 (§1.2), from `mht_port_ctrl.h:28-31` and `mht_port_ctrl.c:539-546,718-732`, proves the 2-bit field is NOT widened: 2500 reuses the 1000 code (value 2), and the get-path has no 2500 case. **Resolution (section 1 wins):** the `mac_link_up` `case SPEED_2500:` writes the existing 1000 code (`QCA8K_PORT_STATUS_SPEED_1000`, 0x2); do not add a distinct code and do not widen the mask. The actual 2.5G rate is realised entirely by the serdes (SGMII+ mode + 312.5 MHz, section 2). This is where the three layers line up: **phylink caps advertise `MAC_2500FD` (§4.D) + PORT_STATUS speed = 1000-code (§1.2) + serdes = SGMII+/312.5 MHz (§2)** all describe the same 2.5G CPU link. For `pcs_get_state`, report 2500 from the serdes, not from the switch PORT_STATUS read (§1.2 get-path can't distinguish it).

**C-4 (CONFIRMED — page register 0x0c). Sections 3 and 4 agree.** Section 4 sets the switch indirect-window page register to 0x0c (`QCA8386_MDIO_PAGE_REG`, `set_page` line 151). Section 3's nsscc driver independently uses the same window: page to phy 0x18 reg 0x0c (`/tmp/nsscc-qca8k.c:28-30,2084`). Two independent code paths confirm the same 0x0c page register at pseudo-PHY 0x18. Section 2's SERDES_CFG read also goes through this window (Layer A). Consistent.

**C-5 (CONFIRMED — register spaces do not overlap; MMD offsets vs the switch map). Section 2's MMD offsets are a different address space from section 1's map, and each section attributes its registers correctly.** Section 1 covers the qca8k-compatible switch-core L2/port block (0x0000-0x0660). Section 2's MMD1/PMAPMD registers (MODE_CTRL 0x11b, CHANNEL0_CFG 0x120, GMII_DATAPASS_SEL 0x180, CALIBRATION4 0x78, USXGMII_RESET 0x18c, MII regs 0/6) are clause-45 registers on the serdes MDIO device (Layer B), not switch-window registers. The one switch-window serdes register, SERDES_CFG 0xC90F014 (bits [9:5] = SGMII_1 address), lives in the mht_reg.h GCC/SerDes block, exactly where section 1 says the QCA8386-specific glue lives (e.g. MDIO_CTRL0 0xC90F03C). No offset conflict. **Implication for the fork:** the serdes MMD access is a SEPARATE path from the DSA regmap (a clause-45 `mdio_device`), so the fork map's `pcs_config` rewrite must add the SERDES_CFG[9:5] resolution + clause-45 device creation (section 2 closing note); this step is implied but not enumerated in §4.D.

**C-6 (CONFIRMED — DT clock/reset names match the nsscc bindings).** The package node consumes nsscc outputs by the exact macro names from `qcom,qca8k-nsscc.h` (clock IDs `NSS_CC_APB_BRIDGE_CLK=2`, `NSS_CC_AHB_CLK=78`, `_SEC_CTRL_AHB_CLK=79`, `_TLMM_CLK=80`, `_TLMM_AHB_CLK=81`, `_CNOC_AHB_CLK=82`, `_MDIO_AHB_CLK=83`, `NSS_CC_GEPHY{0..3}_SYS_CLK=88..91`; reset IDs `NSS_CC_GEPHY_FULL_ARES=65`, `NSS_CC_GEPHY{0..3}_SYS_ARES=52..55`), with `clock-names` matching the driver's `qca8084_package_clk_name[]` (0936-*.patch). The nsscc's own 7 parent inputs are positional (no clock-names) per the driver enum `DT_XO..DT_UNIPHY1_TX312P5M_CLK` (`/tmp/nsscc-qca8k.c:36-44`). Names are internally consistent. The catch is not the names but the missing providers (see C-7).

**C-7 (CONFIRMED gap — nsscc parent clocks unprovided; blocks probe).** Six of the nsscc's seven parents (`qca8k_uniphy0_rx/tx`, `qca8k_uniphy1_rx/tx`, `qca8k_uniphy1_rx312p5m/tx312p5m`) have no in-tree provider in v6.18, and DT_XO (50 MHz) needs a real source. Until stubbed or provided, the nsscc probe cannot complete, which blocks the package clocks, which blocks the four EPHYs. This is the single biggest bring-up blocker. (Section 3 [VALIDATE — hard gap].)

**C-8 (CONFIRMED — package-mode corroboration across sections 2 and 3).** Section 3 recommends `qcom,package-mode = 1` (SGMII_MAC) over the binding default 0 (QXGMII). Section 2 independently establishes the CPU uplink is single-lane SGMII+ 2500 with the XPCS held in reset (MMD1 only), i.e. the switch (SGMII) topology, not the per-channel QXGMII topology. The two agree: use 1. The DSA fork (§4) also models a switch (CPU port + user ports), consistent with switch mode.

**C-9 (CONFIRMED — fork map accounts for every register the other sections reference), with two small carve-outs.** Section 1 switch-core registers are all handled in §4.C/§4.E: MASK_CTRL (read_switch_id), PORT_STATUS (port_set_status/mac_link_up), PORT_HDR_CTL (header-mode step 1905-1913), FORWARD_CTL0 (enable-CPU 1879-1884), FORWARD_CTL1 (flood-to-CPU 1919-1925), PORT_LOOKUP_CTL (membership 1928-1964 + update_port_member/stp). Section 2 serdes registers are delegated into the CPU-port `pcs_config`/`mac_config` rewrite. Carve-outs: (a) HEADER_CTL global ethertype 0x0098 (§1.3 TYPE_VAL/TYPE_LEN) is NOT programmed by qca8k's tag path and is not called out in the fork map — fine unless the tagging decision (§4.E `get_tag_protocol`) requires it; (b) SERDES_CFG 0xC90F014 discovery is implied but not enumerated in §4.D (see C-5). Neither is a missing register, both are noted here so nothing is silently dropped.

**C-10 (CONSISTENT — port count).** Hardware is 7 ports (§1.7); the driver models 5 on SPNMX57 (§4.B/§4.F, `QCA8K_NUM_PORTS 7->5`, `QCA8K_NUM_CPU_PORTS 2->1`). Unused ports 5/6 stay zero in the 7-bit bitmaps. CPU is port 0 only. No conflict; just note the HW-vs-driver port-count difference when reading register bitmaps.

**C-11 (VALIDATE — shared 0x18 page register concurrency).** The DSA switch driver, the nsscc driver, and the package driver all issue MDIO to the same window (page at phy 0x18 reg 0x0c). Each must bracket its page-set + data-access under the bus mdio_lock (as mainline qca8k does with `mutex_lock_nested(&bus->mdio_lock, ...)`) so a page write from one driver cannot interleave between another's page-set and data cycle. The DSA driver additionally caches `mdio_cache.page` and uses `reg_mutex`, but that lock is not shared across drivers. Confirm the locking on HW under concurrent EPHY + switch traffic.

---

## 5. Build order checklist (writing the driver)

Ordered so each step is testable before the next. Steps that need Luca / hardware are flagged; do the rest in full regardless.

**Phase 0 — device tree first (nothing binds without it).**
1. Add the three dt-binding includes (§3). Create `clock-controller@18` (nsscc) with `reg=0x18`, `reset-gpios=<&tlmm 24 GPIO_ACTIVE_LOW>` (this node ONLY, per C-1), the 7 positional parent clocks, and the three `#*-cells`.
2. Provide the parent clocks (C-7 blocker): add `fixed-clock` stubs — `qca8386_ref50m`=50000000, uniphy1 rx/tx312p5m=312500000, the /125M lines=125000000 — or a PCS clock-provider. [VALIDATE on HW.]
3. Create `ethernet-phy-package@1` with the 7 named package clocks, `resets=<&qca8k_nsscc NSS_CC_GEPHY_FULL_ARES>`, `qcom,package-mode=<QCA808X_PCS1_SGMII_MAC_PCS0_SGMII_MAC>` (=1, per C-8), `qcom,phy-addr-fixup=<1 2 3 4 5 6 7>`, and the four `ethernet-phy@1..4` children.
4. Delete the old-stack `qca8084-reset-gpios`, `qcom,qca8084-preinit*`, `reset-delay-us`, `reset-post-delay-us` (`dts:250-274`) and the bare `ethernet-phy@1..4` stubs (`dts:290-303`).
5. Add the switch node under `&mdio1` with `compatible="qca,qca8386"` and its DSA `ports`: port@0 = CPU (fixed-link 2500/full, SGMII+/2500BASEX, `ethernet=<&SoC MAC1>`), port@1..4 = user with `phy-handle=<&ethernet-phy@1..4>`. **Do NOT put `reset-gpios` on this node** (C-1).

**Phase 1 — register-access layer + probe (get `dmesg` to print device id 0x17).**
6. Fork `qca8k.h`: `QCA8386_ID 0x17`, `QCA8386_MDIO_PAGE_REG 0x0c`, `QCA8K_NUM_PORTS 5`, `QCA8K_NUM_CPU_PORTS 1`; shrink the CPU-port enum/arrays; drop `pcs_port_6` and `qca8k_port_to_phy` (§4.F). Leave `QCA8K_PORT_STATUS_SPEED` at GENMASK(1,0) — no 2500 widening (C-3).
7. Copy the verbatim access helpers (§4.A) and apply the one-line `set_page` change (reg 0 -> 0x0c).
8. Rebuild `qca8k_regmap_config` `.max_register` and `.rd_table`/`qca8k_readable_ranges` for the QCA8386 map (§4.A/§4.E). If the DSA driver will read SERDES_CFG 0xC90F014 for serdes discovery (C-5), ensure the readable table/max_register cover 0xC90Fxxx.
9. Fork `qca8k_sw_probe`: keep regmap init and `read_switch_id`; set `num_ports=5`; point ops at the new tables; DROP the reset-gpios block (C-1). Add match_data `{ .id = 0x17 }`, of_match `"qca,qca8386"`, driver name `"qca8386"`.
10. Drop the MDIO-MASTER / eth-mgmt machinery (§4.A) and `qca8k_setup_mdio_bus` (§4.C). [Test: driver binds, `read_switch_id` logs id 0x17. VALIDATE on HW.]

**Phase 2 — switch-core setup + common ops (get L2 forwarding between user ports).**
11. Fork `qca8k_setup` (§4.C): simplify `find_cpu_port` to 0; drop pws/mac_pwr/led/HOL/FC-thresh/MAC06 and `parse_port_config`; keep CPU-enable, mib_init, header-mode ALL on port 0, flood-to-CPU, membership, MTU, ageing.
12. Carry `qca8k-common.c` ops verbatim (§4.E) into `qca8386_switch_ops`; apply the MODIFY items (`readable_ranges`, `port_set_status` auto-follow, `port_change_mtu` CPU-port list). Drop `conduit_state_change`/`connect_tag_protocol`/`get_phy_flags`; decide `get_tag_protocol` (C-9a). [Test: user-port to user-port bridging once EPHYs are up. VALIDATE.]

**Phase 3 — CPU-port phylink + 2.5G serdes (the QCA8386-specific work).**
13. `qca8k_phylink_get_caps`: CPU port advertises 2500BASEX; add `MAC_2500FD` (C-3); fix user-port interfaces; remove port 5.
14. `qca8k_phylink_mac_link_up`: add `case SPEED_2500:` writing the 1000 code (C-3); keep the duplex/flow/MAC-enable tail verbatim.
15. Implement the CPU-port PCS (`pcs_config`/`pcs_link_up`): resolve the serdes address from SERDES_CFG[9:5] (Layer A, §2.1), create a clause-45 `mdio_device` on `priv->bus`, and run the ordered SGMII+ 2500 sequence (§2.2 steps 0-14) with the timings in §2.3 (use the 10 ms analog-reset pulse and the 100 ms calibration timeout). `pcs_get_state` reports 2500 from the serdes (C-3).
16. Rewrite `qca8k_phylink_mac_config`/`mac_select_pcs` for port 0 only (drop port 6, drop PORT0/6_PAD_CTRL). Decide the uniphy1 ownership boundary vs the package driver (C-2). [Test: CPU link comes up at 2.5G full; `ethtool` shows 2500; traffic passes SoC<->switch. VALIDATE.]

**Phase 4 — validation + concurrency.**
17. Confirm the shared 0x18 page register is safe under concurrent switch + EPHY traffic (C-11).
18. Confirm each reused QCA8337-family offset actually matches the QCA8386 core map (MIB base/stride, ageing, MAX_FRAME_SIZE, SGMII_CTRL replacement, etc.) — see Unresolved.
19. Confirm the DT [VALIDATE] items (0x18 uniqueness, gpio24 polarity, phy-addr-fixup 5/6/7, package-mode=1).

**Commands for Luca (hardware in the loop):**
```bash
# After flashing the Phase-1 kernel, confirm the switch is detected as device id 0x17:
dmesg | grep -iE 'qca8386|qca8k|nsscc'
# Expect a line reporting MASK_CTRL device id 0x17 and the driver binding on "qca,qca8386".

# Confirm the four EPHYs enumerate at MDIO 1..4 with PHY ID 0x004dd180:
for a in 1 2 3 4; do echo "phy $a:"; \
  mdio-tool ... /* board MDIO read of PHY id regs 2/3 on &mdio1 addr $a */; done

# After Phase-3, confirm the CPU link negotiates 2.5G:
ethtool <cpu-conduit-netdev>   # look for Speed: 2500Mb/s, Duplex: Full
```
"Worked" = driver binds and logs id 0x17 (Phase 1); user ports bridge (Phase 2); CPU conduit shows 2500Mb/s full and passes traffic (Phase 3).

---

## Unresolved / validate-on-hardware

1. **nsscc parent clocks have no in-tree provider (hard blocker, C-7).** `qca8k_uniphy0_rx/tx`, `qca8k_uniphy1_rx/tx`, `qca8k_uniphy1_rx312p5m/tx312p5m`, and the DT_XO 50 MHz reference need `fixed-clock` stubs or a PCS clock-provider before the nsscc probe can complete. Confirm the resulting link clocking on hardware.
2. **gpio24 reset ownership (conflict C-1).** nsscc must be the sole `reset-gpios` claimant; the DSA switch node/driver must not also claim gpio24. Validate the package leaves reset and the switch comes up after, with no -EBUSY.
3. **gpio24 polarity.** `GPIO_ACTIVE_LOW` is the RE'd guess (vendor flag cell 0); if the package never leaves reset, try `GPIO_ACTIVE_HIGH`.
4. **CPU-uplink serdes ownership (conflict C-2).** Decide whether the DSA driver's CPU-port PCS or the qca8084-package driver (`package-mode`) configures uniphy1/SGMII_1, and confirm they do not fight. This DSA-switch + package-PHY + nsscc coexistence has no mainline precedent.
5. **`qcom,package-mode` value.** Board evidence points to 1 (SGMII_MAC) not the binding default 0 (QXGMII); confirm on hardware.
6. **Device id at MASK_CTRL[15:8] = 0x17.** Confirm the QCA8386 actually reads 0x17 there so the driver binds.
7. **QCA8337-family register offsets vs the QCA8386 core map.** Every reused offset (MIB base 0x1000/stride 0x100, ageing min/max, MAX_FRAME_SIZE, port pad/SGMII_CTRL replacements, readable ranges, regmap `.max_register`) must be confirmed against the QCA8386 map; assumed-compatible is not proven for all of them.
8. **regmap `.max_register` / `.rd_table` bound.** Set to the QCA8386 memory-map end, and (if the DSA driver reads SERDES_CFG at runtime, C-5) extend coverage to 0xC90F014 / the 0xC90Fxxx GCC block.
9. **2500 PORT_STATUS behaviour (resolved in code, verify on HW).** Writing the 1000 speed code (value 2) plus serdes SGMII+/312.5 MHz must yield a real 2.5G link; the switch PORT_STATUS cannot report 2500, so `pcs_get_state` must read the serdes.
10. **CPU tagging decision.** Whether the fixed 2.5G CPU conduit uses `DSA_TAG_PROTO_QCA` (and thus whether HEADER_CTL 0x0098 TYPE_VAL/TYPE_LEN and `get_tag_protocol` need programming, C-9a).
11. **SerDes address discovery method.** SSDK reads SERDES_CFG[9:5] at runtime; the mainline package fixes addresses in DT (`pcs-phy`/`xpcs-phy`). Pick one for `qca8386.c` and confirm the resolved clause-45 address is correct on the live bus.
12. **`reg=0x18` uniqueness and `qcom,phy-addr-fixup` 5/6/7.** Live MDIO scan to confirm nothing else answers at 0x18 and that PCS0/PCS1-PCS/PCS1-XPCS sit at 5/6/7.
13. **Serdes bring-up timings.** SGMII path uses `mdelay(1)` for the analog-reset pulse and adapter/function resets; UQXGMII/mainline use 10 ms and `usleep_range(1000,1100)`. Validate whether 1 ms is enough for stable high-temp lock, or port the safer values.
14. **Shared 0x18 page-register concurrency (C-11).** Confirm bus-mdio_lock bracketing across the DSA, nsscc, and package drivers under concurrent switch + EPHY traffic.
15. **user-port `port_set_status` auto-follow semantics.** The qca8k `port>0 && port<6` LINK_AUTO OR-in assumes internal PHYs; confirm the correct behaviour for external QCA8084-fed user ports on the QCA8386.
16. **MDC-passthrough / MDIO-master bit.** If the QCA8386 has an equivalent to `QCA8K_MDIO_MASTER_EN`, confirm it is cleared once (external EPHYs are driven by the SoC MDIO, not the switch master).