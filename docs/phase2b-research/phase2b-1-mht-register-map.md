## MHT (QCA8386) register map for a DSA driver

### Key structural finding (read this first)

The QCA8386 ("MHT" / Manhattan) switch-core L2/port register block is **register-compatible with the QCA8337 (ssdk "isisc") family**, NOT defined in `mht_reg.h`. The vendor `mht_reg.h` contains **only** the MHT-specific glue: GCC clock/reset, SerDes/UNIPHY config, EPHY address mapping, PTP mux, the pseudo-PHY MDIO master (`MDIO_CTRL0/1`), and NAT/route/flow-congestion blocks — it does **not** define `MASK_CTRL`, `PORT_STATUS`, `PORT_HDR_CTL`, `FORWARD_CTL0/1`, or `PORT_LOOKUP_CTL`.

Proof: every MHT switch-core C file pulls in `isisc_reg.h`, and `mht_reg.h` has none of those symbols.
- `/tmp/ssdk/qca-ssdk-2025.05.30~446db12b/src/hsl/mht/mht_init.c:31` `#include "isisc_reg.h"` (also `mht_port_ctrl.c`, `mht_sec_ctrl.c`, `mht_ip.c`, `mht_nat.c`)
- `grep MASK_CTRL|PORT_STATUS|... mht_reg.h` → 0 hits (only `MHT_DEVICE_ID 0x17` at `mht_reg.h:26`)
- The registers you asked for live in `/tmp/ssdk/qca-ssdk-2025.05.30~446db12b/include/hsl/isisc/isisc_reg.h`

So for a DSA port, **the existing mainline `qca8k` register offsets/bitfields apply almost verbatim**; the QCA8386 delta is entirely in the SerDes/UNIPHY 2.5G bring-up (mht_reg.h + mht_interface_ctrl), not in the DSA switch-core registers.

Mainline comparison file fetched to `srv-openstack:/tmp/qca8k.h` (drivers/net/dsa/qca/qca8k.h @ v6.18; git.kernel.org blocks WebFetch with 403, curl on the build host works).

---

### 1. MASK_CTRL — chip id / revision  (offset 0x0000)

Source: `isisc_reg.h:36` (`MASK_CTL_OFFSET 0x0000`), fields `isisc_reg.h:46-63`. Device id constant `MHT_DEVICE_ID 0x17` at `isisc_reg.h:25` / `mht_reg.h:26`.

| offset | field | bits | meaning | qca8k-diff |
|---|---|---|---|---|
| 0x0000 | SOFT_RST | [31] | full switch soft reset (RW) | qca8k has no define here (resets via GCC) — MHT/isisc only |
| 0x0000 | LOAD_EEPROM | [16] | EEPROM reload (RW) | not in qca8k |
| 0x0000 | DEVICE_ID | [15:8] | chip id, reads **0x17** for QCA8386 | **identical** — `QCA8K_MASK_CTRL_DEVICE_ID_MASK GENMASK(15,8)` (qca8k.h:45) |
| 0x0000 | REV_ID | [7:0] | silicon revision (RO) | **identical** — `QCA8K_MASK_CTRL_REV_ID_MASK GENMASK(7,0)` (qca8k.h:43) |

qca8k: `QCA8K_REG_MASK_CTRL 0x000` (qca8k.h:42). Offset and both bitfields match exactly. Match against device id `0x17` to bind the driver.

---

### 2. PORT_STATUS — per-port MAC force/link/speed  (offset 0x007c + port*4)

Source: `isisc_reg.h:1012-1076`. `PORT_STATUS_OFFSET 0x007c`, stride `E_OFFSET 0x0004`, `NR_E 7` (7 ports). qca8k: `QCA8K_REG_PORT_STATUS(_i) (0x07c + (_i)*4)` (qca8k.h:137) — **identical offset and stride**.

| offset | field | bits | meaning | qca8k-diff |
|---|---|---|---|---|
| 0x07c+p*4 | FLOW_LINK_EN | [12] | 1 = flow control follows autoneg/PHY; 0 = use forced TX/RX_FLOW_EN | **same bit** = `QCA8K_PORT_STATUS_FLOW_AUTO BIT(12)` (qca8k.h:150) |
| | AUTO_RX_FLOW | [11] | RO: resolved RX pause | qca8k leaves bit11 undefined (RO status) |
| | AUTO_TX_FLOW | [10] | RO: resolved TX pause | qca8k names bit10 `QCA8K_PORT_STATUS_LINK_PAUSE BIT(10)` (qca8k.h:149) |
| | LINK_EN | [9] | **force-mode enable**: 1 = MAC follows PHY (auto), 0 = MAC uses the forced speed/duplex/flow in this reg | **same bit** = `QCA8K_PORT_STATUS_LINK_AUTO BIT(9)` (qca8k.h:148). This IS the "force" control — clear it to force the CPU port. |
| | LINK | [8] | RO: link up | `QCA8K_PORT_STATUS_LINK_UP BIT(8)` (qca8k.h:147) — identical |
| | TX_HALF_FLOW_EN | [7] | half-duplex backpressure enable (RW) | not defined in qca8k |
| | DUPLEX_MODE | [6] | forced duplex: 1=full,0=half (RW) | `QCA8K_PORT_STATUS_DUPLEX BIT(6)` (qca8k.h:146) — identical |
| | RX_FLOW_EN | [5] | forced RX pause enable (RW) | `QCA8K_PORT_STATUS_RXFLOW BIT(5)` (qca8k.h:145) — identical |
| | TX_FLOW_EN | [4] | forced TX pause enable (RW) | `QCA8K_PORT_STATUS_TXFLOW BIT(4)` (qca8k.h:144) — identical |
| | RXMAC_EN | [3] | RX MAC enable (RW) | `QCA8K_PORT_STATUS_RXMAC BIT(3)` (qca8k.h:143) — identical |
| | TXMAC_EN | [2] | TX MAC enable (RW) | `QCA8K_PORT_STATUS_TXMAC BIT(2)` (qca8k.h:142) — identical |
| | SPEED_MODE | [1:0] | forced speed, 2-bit code (RW) | mask identical to `QCA8K_PORT_STATUS_SPEED GENMASK(1,0)` (qca8k.h:138); **encoding subtlety below** |

**There are no separate "force speed" / "force duplex" / "force flow" enable bits.** Forcing is done exactly as in qca8k: clear `LINK_EN`/`LINK_AUTO` (bit 9) so the MAC stops following the PHY, then write SPEED_MODE, DUPLEX_MODE, TX/RX_FLOW_EN, and TXMAC_EN/RXMAC_EN directly. (`mht_port_ctrl.c:_mht_port_mac_speed_set` writes SPEED_MODE only via `SW_SET_REG_BY_FIELD(PORT_STATUS, SPEED_MODE, …)` at line 546.)

#### SPEED encoding — how 2500 is handled (the important bit)

The 2-bit SPEED_MODE field is **NOT widened** on the QCA8386. 2.5G reuses the 1000M code:

- `mht_port_ctrl.h:28-31`:
  ```
  #define MHT_PORT_SPEED_10M    0
  #define MHT_PORT_SPEED_100M   1
  #define MHT_PORT_SPEED_1000M  2
  #define MHT_PORT_SPEED_2500M  MHT_PORT_SPEED_1000M   /* == 2 */
  ```
- Set path: `mht_port_ctrl.c:539-546` — `FAL_SPEED_1000 → speed_val=2` and `FAL_SPEED_2500 → speed_val=MHT_PORT_SPEED_2500M` (also 2), both written to `PORT_STATUS.SPEED_MODE`.
- Get path: `mht_port_ctrl.c:718-732` — decodes field 0→10, 1→100, 2→1000; **there is no case for 2500 and no field==3**. The switch-core MAC genuinely cannot tell 2500 from 1000 at this register.

Consequence for the DSA driver: to bring the CPU port up at 2.5G you write `SPEED_MODE = 2` (i.e. the same `QCA8K_PORT_STATUS_SPEED_1000` value, `0x2`), DUPLEX=1, MAC enables on, LINK_AUTO(bit9)=0. The actual 2500 rate comes from the SerDes/UNIPHY: SGMII+ mode (`MHT_UNIPHY_MMD1_SGMII_PLUS_MODE 0x800`, `mht_interface_ctrl.h:49`), XPCS speed code `MHT_UNIPHY_MMD_XPC_SPEED_2500 0x20` (`mht_interface_ctrl.h:116`), and a port clock bump (`mht_interface_ctrl.c:143-144` `UQXGMII_SPEED_2500M_CLK`). qca8k has no 2500 code at all (`QCA8K_PORT_STATUS_SPEED_10/100/1000` only, qca8k.h:139-141), so no source diff is needed on the PORT_STATUS write — you just point the CPU-port phylink at 2500base-X/SGMII and leave SPEED=1000's code.

---

### 3. HEADER_CTL + PORT_HDR_CTL — QCA/Atheros header mode  (0x0098 / 0x009c)

Source: `isisc_reg.h:1082-1126`.

HEADER_CTL (global, offset 0x0098) — the special "Atheros header" ethertype used when header mode is on:

| offset | field | bits | meaning | qca8k-diff |
|---|---|---|---|---|
| 0x0098 | TYPE_LEN | [16] | 1 = header carries a length | **no qca8k define** — qca8k's tag_qca uses a fixed 2-byte length-style header and never programs an ethertype here |
| 0x0098 | TYPE_VAL | [15:0] | header ethertype value | not used by qca8k |

PORT_HDR_CTL (per-port, offset 0x009c + port*4, NR_E 7):

| offset | field | bits | meaning | qca8k-diff |
|---|---|---|---|---|
| 0x09c+p*4 | IPG_DEC_EN | [5] | shrink IPG when header added | not in qca8k |
| | LOOPBACK_EN | [4] | port loopback | not in qca8k |
| | RXHDR_MODE | [3:2] | RX header mode: 0=none,1=mgmt-only,2=all | **identical** = `QCA8K_PORT_HDR_CTRL_RX_MASK GENMASK(3,2)` (qca8k.h:152) |
| | TXHDR_MODE | [1:0] | TX header mode: 0=none,1=mgmt-only,2=all | **identical** = `QCA8K_PORT_HDR_CTRL_TX_MASK GENMASK(1,0)` (qca8k.h:153) |

Mode values match qca8k exactly: `QCA8K_PORT_HDR_CTRL_NONE 0`, `_MGMT 1`, `_ALL 2` (qca8k.h:154-156). For the CPU port set TX+RX = ALL (2); leave user ports = NONE (0). qca8k register/stride `QCA8K_REG_PORT_HDR_CTRL(_i) (0x9c + (_i*4))` (qca8k.h:151) — identical.

---

### 4. FORWARD_CTL0 (= GLOBAL_FW_CTRL0) — CPU port enable  (offset 0x0620)

Source: `isisc_reg.h:1787-1846`. `FORWARD_CTL0_OFFSET 0x0620`.

| offset | field | bits | meaning | qca8k-diff |
|---|---|---|---|---|
| 0x0620 | CPU_PORT_EN | [10] | master enable for the CPU port | **identical** = `QCA8K_GLOBAL_FW_CTRL0_CPU_PORT_EN BIT(10)` (qca8k.h:245) |
| 0x0620 | MIRROR_PORT_NUM | [7:4] | monitor/mirror dest port | **identical** = `QCA8K_GLOBAL_FW_CTRL0_MIRROR_PORT_NUM GENMASK(7,4)` (qca8k.h:246) |

(Other bits present in ssdk but not needed by DSA: ARP_CMD[27:26], IP_NOT_FOUND[25:24], ARP_NOT_FOUND[23:22], HASH_MODE[21:20], NAT/SP_NOT_FOUND_DROP[17]/[16], IGMP_LEAVE_DROP[14], ARL_UNI/MUL_LEAKY[13]/[12], MANAGE_VID_VIO_DROP_EN[11], PPPOE_RDT_EN[8], IGMP_COPY_EN[3], RIP_CPY_EN[2] — `isisc_reg.h:1794-1866`.) qca8k: `QCA8K_REG_GLOBAL_FW_CTRL0 0x620` (qca8k.h:244).

---

### 5. FORWARD_CTL1 (= GLOBAL_FW_CTRL1) — flood-to-CPU / dest-port bitmaps  (offset 0x0624)

Source: `isisc_reg.h:1874-1898`. `FORWARD_CTL1_OFFSET 0x0624`.

| offset | field | bits | meaning | qca8k-diff |
|---|---|---|---|---|
| 0x0624 | IGMP_DP | [30:24] (BOFFSET 24, BLEN 7) | IGMP/mgmt frame dest-port bitmap | qca8k `QCA8K_GLOBAL_FW_CTRL1_IGMP_DP_MASK GENMASK(30,24)` (qca8k.h:248) — **identical** |
| 0x0624 | BC_FLOOD_DP | [22:16] (BOFFSET 16, BLEN 7) | broadcast flood dest-port bitmap | qca8k `..._BC_DP_MASK GENMASK(22,16)` (qca8k.h:249) — **identical** |
| 0x0624 | MUL_FLOOD_DP | [14:8] (BOFFSET 8, BLEN 7) | unknown-multicast flood dest-port bitmap | qca8k `..._MC_DP_MASK GENMASK(14,8)` (qca8k.h:250) — **identical** |
| 0x0624 | UNI_FLOOD_DP | [6:0] (BOFFSET 0, BLEN 7) | unknown-unicast flood dest-port bitmap | qca8k `..._UC_DP_MASK GENMASK(6,0)` (qca8k.h:251) — **identical** |

Each is a 7-bit **port bitmap** (bit N = port N). To flood unknowns to the CPU, set bit 0 in the relevant field. qca8k: `QCA8K_REG_GLOBAL_FW_CTRL1 0x624` (qca8k.h:247). Fully identical.

---

### 6. PORT_LOOKUP_CTL (= PORT_LOOKUP_CTRL) — per-port member/state  (offset 0x0660 + port*0xc)

Source: `isisc_reg.h:1948-2007`. `PORT_LOOKUP_CTL_OFFSET 0x0660`, stride `E_OFFSET 0x000c`, `NR_E 7`. qca8k: `QCA8K_PORT_LOOKUP_CTRL(_i) (0x660 + (_i)*0xc)` (qca8k.h:252) — **identical offset and 0xc stride**.

| offset | field | bits | meaning | qca8k-diff |
|---|---|---|---|---|
| 0x660+p*0xc | MULTI_DROP_EN | [31] | drop multicast | not in qca8k |
| | UNI_LEAKY_EN | [28] | unicast leaky | not in qca8k |
| | MUL_LEAKY_EN | [27] | multicast leaky | not in qca8k |
| | ARP_LEAKY_EN | [26] | ARP leaky | not in qca8k |
| | ING_MIRROR_EN | [25] | ingress mirror | not in qca8k |
| | PORT_LOOP_BACK | [21] | loopback | not in qca8k |
| | LEARN_EN | [20] | SA learning enable | qca8k sets learn via `QCA8K_PORT_LOOKUP_LEARN BIT(20)` (present in qca8k source; same bit) |
| | PORT_STATE | [18:16] (BOFFSET 16, BLEN 3) | STP state: 0=disabled,1=blocking,2=listening,3=learning,(4..=forwarding per HW) | **identical** = `QCA8K_PORT_LOOKUP_STATE_MASK GENMASK(18,16)` with STATE_DISABLED/BLOCKING/LISTENING/LEARNING = 0/1/2/3 (qca8k.h:259-263) |
| | FORCE_PVLAN | [10] | force port VLAN | not exposed in qca8k.h |
| | DOT1Q_MODE | [9:8] (BLEN 2) | VLAN mode: 0=none,1=fallback,2=check,3=secure | **identical** = `QCA8K_PORT_LOOKUP_VLAN_MODE_MASK GENMASK(9,8)`, values 0/1/2/3 (qca8k.h:254-258) |
| | PORT_VID_MEM | [6:0] (BOFFSET 0, BLEN 7) | **port membership bitmap** (which ports this port may forward to) | **identical** = `QCA8K_PORT_LOOKUP_MEMBER GENMASK(6,0)` (qca8k.h:253) |

Membership and STP-state semantics are byte-for-byte the same as qca8k.

---

### 7. Port bitmask width / port numbering

- **7 ports total**, indices 0-6. Confirmed by every switch-core register's `NR_E 7` (PORT_STATUS `isisc_reg.h:1016`, PORT_HDR_CTL `:1106`, PORT_LOOKUP_CTL `:1952`) and by the 7-bit-wide port-bitmap fields (`PORT_VID_MEM` BLEN 7, all FORWARD_CTL1 `_DP` BLEN 7). Matches `QCA8K_NUM_PORTS 7` (qca8k.h:21).
- **Bit index = port number** in every bitmap (member, flood DPs). Bit 0 = port 0.
- On the SPNMX57 specifically (authoritative DTS `ipq5018-spnmx57.dts`):
  - **Port 0 = CPU port**, forced 2.5G/full, no PHY, SGMII+ uplink to IPQ5018 MAC1 — `dts:56-58,63-64` (`switch_cpu_bmp = <ESS_PORT0>`), header intro `dts:21`.
  - **Ports 1-4 = the four user/LAN ports** (QCA8084 EPHYs at MDIO addr 1-4) — `dts:59` (`switch_lan_bmp = <(ESS_PORT1|ESS_PORT2|ESS_PORT3|ESS_PORT4)>`), PHY nodes `dts:290-303` (reg 0x01..0x04), switch port nodes `dts:90-105`.
  - **No WAN port**, `switch_wan_bmp = <0>` (`dts:60`). Ports 5 and 6 are unused on this board.
- qca8k treats port 0 and port 6 as the two possible CPU ports (`enum { QCA8K_CPU_PORT0, QCA8K_CPU_PORT6 }`, qca8k.h:389-390; `QCA8K_NUM_CPU_PORTS 2`, qca8k.h:22). SPNMX57 uses **CPU_PORT0** only.

---

### C #define block worth copying (DSA-relevant subset)

Because the switch core is qca8k-compatible, the mainline `qca8k.h` values are directly reusable; only the identity constant and the 2500 note differ:

```c
/* Chip identity — bind the DSA driver on this */
#define MHT_QCA8386_DEVICE_ID       0x17   /* mht_reg.h:26 / isisc_reg.h:25; reads in MASK_CTRL[15:8] */

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
#define QCA8K_REG_GLOBAL_FW_CTRL1   0x624    /* IGMP[30:24] BC[22:16] MC[14:8] UC[6:0], each a 7-bit port bitmap */
#define QCA8K_PORT_LOOKUP_CTRL(p)   (0x660 + (p)*0xc) /* MEMBER[6:0], VLAN_MODE[9:8], STATE[18:16] */
#define QCA8K_NUM_PORTS             7        /* p0 = CPU, p1..p4 = QCA8084 EPHYs on SPNMX57 */
```

### Net advice for the port
For the DSA switch-core registers there is effectively **nothing to reverse-engineer** — reuse the mainline `qca8k` register layer as-is, match on device id `0x17`, and drive port 0 as the CPU port with header mode ALL and forced 1000-code speed. The genuinely QCA8386-specific work (which is why `mht_reg.h`/`mht_interface_ctrl.h` exist) is: the indirect MDIO register window (`mht_reg.h` MDIO_CTRL0 0xC90F03C, page via pseudo-PHY), and the SerDes/UNIPHY SGMII+ 2.5G bring-up for the CPU uplink — none of which is a DSA switch-core register.