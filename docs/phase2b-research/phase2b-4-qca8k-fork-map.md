## Fork map: `drivers/net/dsa/qca/qca8386.c` from mainline v6.18 `qca8k`

All line numbers below are from the raw v6.18 files fetched from git.kernel.org and saved locally for exact line-addressing:
- `qca8k-8xxx.c` (2228 lines) — the MDIO-attached driver body
- `qca8k-common.c` (1258 lines) — the switch-core ops
- `qca8k.h` (596 lines) — regs, structs, prototypes

Grounding for the QCA8386 deltas (read on the build host):
- Page write goes to pseudo-PHY `0x18`, and in the qca8k family that page register is `reg 0`: `qca-ssdk .../src/hsl/isisc/isisc_reg_access.c:63-64` (`phy_addr = 0x18; phy_reg = 0x0;`). This is exactly what mainline `qca8k_set_page` does. The QCA8386 (MHT) page register is `0x0c` (Phase-1 RE fact); the MHT bit-bang path in the SSDK is routed through the external mdio driver's `sw_read/sw_write` callbacks (`.../src/init/ssdk_plat.c:333-357`), so the literal `0x0c` offset is not in this SSDK snapshot — it comes from the Phase-1 reverse engineering and the live bus.
- CPU uplink is 2.5G SGMII+ / UQXGMII: `PORT_SGMII_PLUS` (`.../src/shell_lib/shell_io.c:1691-1693`), `UQXGMII_SPEED_2500M_CLK` throughout `.../src/init/ssdk_mht_clk.c`.
- Board topology (authoritative): `ipq5018-spnmx57.dts:45-66` (access mode mdio, on mdio1, CPU `port@0` `forced-speed=<2500>` `forced-duplex=<1>`, `switch_mac_mode=<MAC_MODE_SGMII_PLUS>`), `:90-108` (four QCA8084 EPHYs at MDIO 1..4), `:277-312` (real phy nodes `ethernet-phy@1..4`, PHY ID `0x004dd180`), `:252-272` (package reset `qca8084-reset-gpios = <&tlmm 24 GPIO_ACTIVE_LOW>`). Note the vendor comment at `:51-53` that `MAC_MODE_SGMII_PLUS` happens to enumerate to `0x0c` — that is unrelated to the page register also being `0x0c`.

---

### A. Register-access layer (`qca8k-8xxx.c`)

| Symbol | Lines | Verdict | Change |
|---|---|---|---|
| `qca8k_split_addr` | 27-38 | **COPY VERBATIM** | Identical addressing math: word=reg>>1, `r1=&0x1e`, `r2=(>>5)&0x7`, `page=(>>8)&0x3ff`. Confirmed identical in `isisc_reg_access.c:60-92`. QCA8386 uses the same 0x10-0x1f data window / 0x18 page pseudo-PHYs. |
| `qca8k_mii_write_lo` | 40-53 | **COPY VERBATIM** | Pure 16-bit MDIO write, chip-independent. |
| `qca8k_mii_write_hi` | 55-68 | **COPY VERBATIM** | ditto. |
| `qca8k_mii_read_lo` | 70-88 | **COPY VERBATIM** | ditto. |
| `qca8k_mii_read_hi` | 90-108 | **COPY VERBATIM** | ditto. |
| `qca8k_mii_read32` | 110-130 | **COPY VERBATIM** | lo then hi+1 assembly of the 32-bit value. |
| `qca8k_mii_write32` | 132-139 | **COPY VERBATIM** | lo then hi+1. |
| `qca8k_set_page` | 141-161 | **MODIFY (one line)** | Line 151 `ret = bus->write(bus, 0x18, 0, page);` → change the register from `0` to `0x0c`: `bus->write(bus, 0x18, QCA8386_MDIO_PAGE_REG, page)` with `#define QCA8386_MDIO_PAGE_REG 0x0c`. Keep the `page == *cached_page` fast-path (148-149), the error log, and the `usleep_range(1000, 2000)` (159). This is THE defining register-access delta. |
| `qca8k_read_mii` | 428-448 | **COPY VERBATIM** | Calls split_addr → set_page → `qca8k_mii_read32(bus, 0x10 | r2, r1, val)`. The `0x10|r2` data window is confirmed identical (`isisc_reg_access.c:84,143`). It transitively picks up the modified `set_page`. |
| `qca8k_write_mii` | 450-470 | **COPY VERBATIM** | ditto (`qca8k_mii_write32(bus, 0x10 | r2, r1, val)`). |
| `qca8k_regmap_update_bits_mii` | 472-501 | **COPY VERBATIM** | RMW over the same window. |
| `qca8k_bulk_read` | 503-523 | **COPY VERBATIM** | Falls back to per-reg `qca8k_read_mii` when no mgmt conduit (511-519); safe to keep — the eth-mgmt branch stays dormant until/unless a conduit registers. |
| `qca8k_bulk_gather_write` / `qca8k_bulk_write` | 525-555 | **COPY VERBATIM** | same reasoning. |
| `qca8k_regmap_update_bits` | 557-566 | **COPY VERBATIM** | tries eth, falls back to `_mii`. |
| `qca8k_regmap_config` ("bus-less" regmap) | 568-584 | **MODIFY (fields)** | Keep `.reg_bits=16, .val_bits=32, .reg_stride=4` (same indirect word scheme), keep `.read/.write/.reg_update_bits = qca8k_bulk_read/write/update_bits`, keep `.disable_locking=true`, `.cache_type=REGCACHE_NONE`, `.max_raw_read=32`, `.use_single_write=true` (ATU-bug workaround). **Change** `.max_register = 0x16ac` (that is the QCA8337 "end MIB Port6" bound, line 572) and `.rd_table = &qca8k_readable_table` (QCA8337 ranges, `common.c:81-102`) to the QCA8386 memory-map bound and readable table. This regmap is instantiated bus-less at `sw_probe` line 2079-2080 `devm_regmap_init(&mdiodev->dev, NULL, priv, &qca8k_regmap_config)` (the `NULL` = no `regmap_bus`) — keep that call shape verbatim. |

The **entire MDIO-MASTER / eth-mgmt PHY machinery is DROPPED** (external QCA8084 PHYs, so no internal PHY access through the switch): `qca8k_phy_eth_command` (622-791), `qca8k_mdio_busy_wait` (793-813), `qca8k_mdio_write` (815-851), `qca8k_mdio_read` (853-895), `qca8k_internal_mdio_write/read` (897-928), `qca8k_legacy_mdio_write/read` (930-944), `qca8k_mdio_register` (946-992), `qca8k_setup_mdio_bus` (994-1061). The eth rw/ack/mib-autocast helpers (163-620, 1644-1727, 1752-1790) may be kept dormant or dropped; drop them for a lean first bring-up since the SPNMX57 CPU link is a plain fixed 2.5G conduit, not a tag-mgmt conduit.

---

### B. Probe / ID / driver registration

**`qca8k_read_switch_id`** — `common.c:1230-1257`. **MODIFY (match value only, body verbatim).** Reads `QCA8K_REG_MASK_CTRL` (0x000), device id = `QCA8K_MASK_CTRL_DEVICE_ID` = bits 15:8 (`qca8k.h:42-46`), revision = bits 7:0. The comparison is `id != priv->info->id` (1244) — so the change is data-driven: add `#define QCA8386_ID 0x17` and a match_data with `.id = 0x17`. Body stays verbatim. *Verify on HW that QCA8386 exposes device-id at MASK_CTRL 0x000 bits 15:8 = 0x17; 0x17 fits the 8-bit field.*

**`qca8k_sw_probe`** — `qca8k-8xxx.c:2048-2112`. **MODIFY.** Keep verbatim: priv alloc (2057), `priv->bus = mdiodev->bus` (2061), `priv->info = of_device_get_match_data` (2063), the **reset_gpio block (2065-2076)** — this drives the DT `reset-gpios` (the tlmm gpio24 active-low QCA8386 package reset, `dts:252-272`) with the 20 ms low pulse, keep as-is; regmap init (2079-2084) pointing at the modified config; `priv->mdio_cache.page = 0xffff` (2086, invalidate page cache); `qca8k_read_switch_id` (2089); ds alloc + `ds->priv/ops/phylink_mac_ops` wiring (2093-2107); `reg_mutex` init; `dsa_register_switch` (2111). **Change:** `priv->ds->num_ports = QCA8K_NUM_PORTS` (2104) → 5 (CPU port0 + user 1..4); point `ds->ops` at the new `qca8386_switch_ops` and `ds->phylink_mac_ops` at the new `qca8386_phylink_mac_ops`. The mgmt/mib mutex+completion inits (2097-2101) are harmless to keep even if the eth path is dropped, but drop them if you drop those structs.

**mdio_driver registration** — `qca8k-8xxx.c`: `qca8k_of_match` (2203-2209), `qca8kmdio_driver` (2211-2220), `mdio_module_driver(qca8kmdio_driver)` (2222), plus the `qca8k_match_data` table (2184-2201) and `qca8xxx_ops` (2180-2182). **MODIFY.** Replace the of_match table with a single `{ .compatible = "qca,qca8386", .data = &qca8386 }`; define `static const struct qca8k_match_data qca8386 = { .id = QCA8386_ID, .mib_count = <n>, .ops = ... }`; set `.mdiodrv.driver.name = "qca8386"`. `qca8xxx_ops.autocast_mib` is only used by the eth-mgmt MIB path — drop it if you drop that path (then `.ops` can be NULL, but guard `common.c:502-503`). Keep `SIMPLE_DEV_PM_OPS` / suspend / resume (2141-2178) verbatim, and `qca8k_sw_remove`/`qca8k_sw_shutdown` (2114-2139) verbatim (they only touch `qca8k_port_set_status` and DSA teardown), noting the `for (i=0;i<QCA8K_NUM_PORTS;i++)` loop bound (2123) follows the reduced port count.

---

### C. `qca8k_setup` — `qca8k-8xxx.c:1832-2002`

Keep the overall structure; the register OFFSETS it writes are QCA8337-family (`qca8k.h`) and every one must be validated against the QCA8386 map before trusting it (see Unresolved). Sub-step verdicts:

- `qca8k_find_cpu_port` (call 1840; def 1090-1104) — **MODIFY/simplify.** Only port 0 is CPU (`dts:58 switch_cpu_bmp=<ESS_PORT0>`); the port-6 fallback (1100-1101) is dead. Return 0 (or keep the port-0 branch only).
- `qca8k_parse_port_config` (call 1847; def 1146-1242) — **DROP or stub.** It parses RGMII/SGMII delays for CPU ports 0 and 6 (loop 1156-1159). QCA8386's uplink is fixed 2.5G SGMII+ with no RGMII pad delays; none of this applies. Replace with nothing, or a minimal SGMII+ parse.
- `qca8k_setup_mdio_bus` (call 1851; def 994-1061) — **DROP entirely.** This is the internal-vs-external MDIO-master decision and internal bus registration. The four EPHYs are EXTERNAL QCA8084s on the SoC `mdio1` bus (`dts:45-108, 277-312`), bound by the mainline QCA8084 PHY-package driver (patches `0930-*.patch`..`0941-*.patch`) and referenced from the switch user ports via `phy-handle`. Do NOT enable `QCA8K_MDIO_MASTER_EN`. (If the QCA8386 has an equivalent "MDC passthrough" bit, clear it once instead — verify against the QCA8386 map.)
- `qca8k_setup_of_pws_reg` (call 1855; def 1106-1144) — **DROP.** QCA8327/8337 package-148 / power-on-sel / led-open-drain, gated on `switch_id == QCA8K_ID_QCA8327` (1118); never matches 0x17.
- `qca8k_setup_mac_pwr_sel` (call 1859; def 1063-1088) — **DROP.** ipq8064/8065 RGMII-1.8V only; not IPQ5018.
- `qca8k_setup_led_ctrl` (call 1863; in `qca8k_leds.c`, not fetched) — **DROP for bring-up.** QCA8386 LED register map differs; port later.
- `qca8k_setup_pcs` (calls 1867-1868; def 1633-1642) — **MODIFY.** Keep the container/ops wiring but set up only the CPU-side PCS (`pcs_port_0`, port 0); drop `pcs_port_6`.
- MAC06-exchange disable (1871-1876) — **DROP.** `QCA8K_REG_PORT0_PAD_CTRL` bit31 is QCA8337-specific.
- Enable-CPU-port (1879-1884), `qca8k_mib_init` (1887), disable-forwarding/MAC loops (1892-1902), QCA-header-mode on CPU ports (1905-1913), unknown-frame flood to CPU (1919-1925), CPU↔user port-lookup membership (1928-1964), MAX_FRAME_SIZE (1987), `qca8k_fdb_flush` (1992), ageing min/max (1995-1996), `num_lag_ids` (1999) — **KEEP the logic**, but each touches a `qca8k.h` register offset that must be confirmed for QCA8386. The `dsa_switch_for_each_*` iterators auto-scale with `num_ports=5`.
- HOL fixup (1966-1974; def `qca8k_setup_hol_fixup` 1792-1830) — **DROP.** Gated on `QCA8K_ID_QCA8337` (1972).
- GLOBAL_FC_THRESH (1977-1984) — **DROP.** Gated on `QCA8K_ID_QCA8327` (1977).

---

### D. phylink

**`qca8k_phylink_get_caps`** — `qca8k-8xxx.c:1400-1433`. **MODIFY.**
- Port 0 / CPU (1404-1408): currently `phy_interface_set_rgmii` + SGMII. Replace with the 2.5G uplink set — `__set_bit(PHY_INTERFACE_MODE_2500BASEX, ...)` (and optionally SGMII); drop RGMII.
- Ports 1-5 "internal PHY" branch (1410-1420) sets `GMII`/`INTERNAL` — **wrong for QCA8386.** User ports 1..4 face external QCA8084s. Set the MAC↔QCA8084 interface(s) actually used (SGMII / 2500BASEX per the QCA8084 serdes; UQXGMII in vendor terms). Remove port 5.
- `mac_capabilities` (1431-1432): `MAC_ASYM_PAUSE | MAC_SYM_PAUSE | MAC_10 | MAC_100 | MAC_1000FD` → **add `MAC_2500FD`**.

**`qca8k_phylink_mac_select_pcs`** — 1285-1313. **MODIFY.** Keep the `container_of` pattern; map port 0 → `pcs_port_0`; drop the port-6 case; add per-user-port PCS if the QCA8084 links run 2500BASEX and need switch-side PCS.

**`qca8k_phylink_mac_config`** — 1315-1398. **MODIFY (largely rewrite).** The port dispatch (1328-1362) and the RGMII/SGMII pad programming write `QCA8K_REG_PORT0_PAD_CTRL`/`PORT6_PAD_CTRL` (QCA8337 pad regs). For QCA8386 this becomes the MAC/uniphy interface bring-up for SGMII+/2500 on the CPU port (the SSDK does this in `src/hsl/mht/mht_interface_ctrl.c`). Keep the "internal PHY, nothing to do" early-return shape for user ports only if the QCA8084 side is fully PCS-driven; otherwise add the user-port MAC config. Port 6 handling (1347-1358) is dropped.

**`qca8k_phylink_mac_link_up`** — 1445-1487. **MODIFY (2500 support + CPU force).**
- Speed switch (1459-1472) only encodes 10/100/1000 via `QCA8K_PORT_STATUS_SPEED_10/100/1000`. `QCA8K_PORT_STATUS_SPEED` is `GENMASK(1,0)` with only values 0/1/2 defined (`qca8k.h:138-141`) — **there is no 2500 code.** Add a `case SPEED_2500:` and the QCA8386 speed-2500 encoding for `QCA8K_REG_PORT_STATUS` (verify the field width/value on the QCA8386 map).
- The CPU port (port 0) runs fixed 2500/full (`dts:64-66 forced-speed=<2500> forced-duplex=<1>`): with `phylink_autoneg_inband(mode)` false, DSA calls this with the fixed-link speed, so once `SPEED_2500` is encoded the existing `DUPLEX_FULL`/`RXFLOW`/`TXFLOW`/`TXMAC|RXMAC` logic (1474-1486) applies. Keep 1474-1486 verbatim.
- `qca8k_phylink_mac_link_down` (1435-1443) — **COPY VERBATIM** (just `qca8k_port_set_status(priv, dp->index, 0)`).

**PCS ops** — `qca8k_pcs_get_state` (1494-1532) **MODIFY** (add SPEED_2500 decode, same missing-code issue as above). `qca8k_pcs_config` (1534-1621) **MODIFY** — the SGMII_CTRL programming (`QCA8K_REG_SGMII_CTRL` 1569-1591, PHY/MAC/BASEX mode) is QCA8337-specific; replace with QCA8386 SGMII+/2500 serdes config. `qca8k_pcs_an_restart` (1623-1625) empty — verbatim. `qca8k_pcs_ops`/`qca8k_setup_pcs` (1627-1642) — keep shape.

**`qca8k_phylink_mac_ops`** — 2004-2009. Keep as the template for `qca8386_phylink_mac_ops`, pointing at the modified callbacks.

---

### E. `qca8k-common.c` switch-core ops — reusable UNCHANGED?

Register/RMW wrappers and the whole FDB/ATU/VTU/VLAN/STP/bridge/port-lookup/MIB/MDB/mirror/LAG body are **logic-reusable UNCHANGED**, with two cross-cutting caveats: (1) they read register offsets/field masks from `qca8k.h` that are QCA8337-family and must be confirmed to match the QCA8386 core map; (2) loops bounded by `QCA8K_NUM_PORTS` (`qca8k.h:21`, value 7) must follow the reduced port count — cleanest is to redefine `QCA8K_NUM_PORTS` to 5 (the `GENMASK`-based port masks still work with unused high bits zero).

**Copy VERBATIM (pure logic, no chip-specific branching):**
- `qca8k_read` / `qca8k_write` / `qca8k_rmw` — 66-79
- `qca8k_busy_wait` — 104-110
- FDB/ATU core: `qca8k_fdb_read` 112-138, `qca8k_fdb_write` 140-162, `qca8k_fdb_access` 164-198, `qca8k_fdb_next` 200-211, `qca8k_fdb_add` 213-224, `qca8k_fdb_del` 226-237, `qca8k_fdb_flush` 239-244, `qca8k_fdb_search_and_insert` 246-281, `qca8k_fdb_search_and_del` 283-323
- VTU/VLAN core: `qca8k_vlan_access` 325-356, `qca8k_vlan_add` 358-394, `qca8k_vlan_del` 396-437 (loop 415 uses NUM_PORTS)
- ethtool: `qca8k_get_strings` 480-491, `qca8k_get_ethtool_stats` 493-524, `qca8k_get_sset_count` 526-534
- learning/STP: `qca8k_port_configure_learning` 560-573, `qca8k_port_stp_state_set` 575-608
- bridge/port-lookup: `qca8k_update_port_member` 610-654 (loop 619 uses NUM_PORTS), `qca8k_port_pre_bridge_flags` 656-664, `qca8k_port_bridge_flags` 666-695, `qca8k_port_bridge_join` 697-705, `qca8k_port_bridge_leave` 707-718
- `qca8k_port_fast_age` 720-727, `qca8k_set_ageing_time` 729-747
- `qca8k_port_enable` 749-761, `qca8k_port_disable` 763-769
- FDB DSA ops: `qca8k_port_fdb_insert` 814-823, `qca8k_port_fdb_add` 825-833, `qca8k_port_fdb_del` 835-846, `qca8k_port_fdb_dump` 848-869
- MDB: `qca8k_port_mdb_add` 871-884, `qca8k_port_mdb_del` 886-898
- mirror: `qca8k_port_mirror_add` 900-953, `qca8k_port_mirror_del` 955-989
- VLAN DSA ops: `qca8k_port_vlan_filtering` 991-1009, `qca8k_port_vlan_add` 1011-1039, `qca8k_port_vlan_del` 1041-1052
- LAG: `qca8k_lag_can_offload` 1054-1089, `qca8k_lag_setup_hash` 1091-1137, `qca8k_lag_refresh_portmap` 1139-1206, `qca8k_port_lag_join` 1208-1222, `qca8k_port_lag_leave` 1224-1228
- `qca8k_mib_init` 439-464 and `qca8k_set_mac_eee` 536-558 — reusable as-is, contingent only on MIB/EEE reg offsets.
- `ar8327_mib[]` MIB descriptor table 22-64 — reusable if the QCA8386 per-port MIB layout matches (verify `QCA8K_PORT_MIB_COUNTER` base 0x1000/stride 0x100, `qca8k.h:338`).

**MODIFY:**
- `qca8k_readable_ranges` / `qca8k_readable_table` — 81-102. QCA8337 ranges incl. MIB Port0-6; rebuild for the QCA8386 map (this is the `.rd_table` used by the regmap config in §A).
- `qca8k_port_set_status` — 466-478. The `port > 0 && port < 6` special-case that OR-in `QCA8K_PORT_STATUS_LINK_AUTO` (471-472) assumes internal PHYs on ports 1-5. QCA8386 user ports 1..4 face external QCA8084s (link fed by phylink), so the auto-follow bit range/semantics must change; and the CPU port(s) list changes from {0,6} to {0}.
- `qca8k_port_change_mtu` — 771-807. Hardcodes CPU ports 0 AND 6 for the off/on dance (790-804); reduce to port 0. `qca8k_port_max_mtu` 809-812 verbatim (returns `QCA8K_MAX_MTU`).
- `qca8k_read_switch_id` — 1230-1257 (match `0x17`, see §B).

**`qca8k_switch_ops`** table — `qca8k-8xxx.c:2011-2046`. Reuse as the template for `qca8386_switch_ops`: the FDB/VLAN/bridge/mirror/LAG/MTU/STP/ethtool/EEE entries all point at the reusable `common.c` functions above and can be carried over verbatim. Drop `conduit_state_change`/`connect_tag_protocol` (2044-2045) and `get_phy_flags` (`qca8k-8xxx.c:1729-1743`, communicates QCA8337 switch revision to the internal PHY — meaningless for external QCA8084) if the eth-mgmt/internal-PHY paths are dropped; keep `get_tag_protocol` (1745-1750) only if the CPU link actually uses `DSA_TAG_PROTO_QCA` tagging (on a plain fixed 2.5G conduit you likely need a real tagging decision here — verify).

---

### F. Header (`qca8k.h`) changes
- `QCA8K_NUM_PORTS` 7 → 5 (line 21); `QCA8K_NUM_CPU_PORTS` 2 → 1 (line 22).
- Add `#define QCA8386_ID 0x17` alongside 27-30.
- Add `#define QCA8386_MDIO_PAGE_REG 0x0c` (used by the modified `set_page`).
- Add a `QCA8K_PORT_STATUS_SPEED_2500` encoding + widen/verify `QCA8K_PORT_STATUS_SPEED` (138-141) against the QCA8386 map.
- The `QCA8K_CPU_PORT0/QCA8K_CPU_PORT6` enum (388-391) and `ports_config` CPU-port arrays (413-414) shrink to one CPU port.
- `struct qca8k_priv` (444-471): keep; drop `pcs_port_6` (468) and the internal/eth-mgmt members if those paths are dropped. Keep `mdio_cache.page` (417-423, 466) — still needed for the page cache.
- `qca8k_port_to_phy` (486-498) — internal-PHY mapping, unused once the MDIO-master path is dropped; remove.
