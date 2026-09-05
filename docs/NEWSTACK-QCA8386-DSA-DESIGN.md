# QCA8386 ("Manhattan"/MHT) DSA driver on the current OpenWrt ethernet stack — implementation design

**Target:** OpenWrt `main`, kernel **6.18.44**, target `qualcommax`, subtarget `ipq50xx`, board `linksys,spnmx57`.
**Build tree:** `srv-openstack:/tmp/owrt-main` (tmpfs — volatile; board files backed up under [`newstack/`](../newstack/)).
**Goal of Phase 2:** replace the Phase‑1 vendor `qca-ssdk` blob (old stack, kernel 6.12) with a mainline‑style DSA driver + PHY/PCS/clock drivers on the new stack.

> This is the output of the Phase‑2 research pass (2026‑09‑05). It is the roadmap for the implementation. Everything cited was read from real in‑tree sources on `srv-openstack`, except the mainline `qca8k` DSA driver (kernel not yet extracted in the tree) which was read from `git.kernel.org` at the matching `v6.18` tag and lands at `build_dir/.../linux-6.18.44/drivers/net/dsa/qca/` once built.

Reference sources (on `srv-openstack`):
- Working **Phase‑1 old‑stack DTS** (authoritative topology/GPIO/addresses): `/home/luca/spnmx57/openwrt/target/linux/qualcommax/files/arch/arm64/boot/dts/qcom/ipq5018-spnmx57.dts`
- Vendor **qca-ssdk** (reference only, NOT used on the new stack): `/tmp/ssdk/qca-ssdk-2025.05.30~446db12b/`

---

## 0. The hardware, precisely

```
 IPQ5018 MAC1 ── UNIPHY0/1 ──SGMII+ 2.5G (forced)── QCA8386 port0 (CPU)
                                                     QCA8386 port1 ── QCA8084 EPHY @ MDIO 1
                                                     QCA8386 port2 ── QCA8084 EPHY @ MDIO 2
                                                     QCA8386 port3 ── QCA8084 EPHY @ MDIO 3
                                                     QCA8386 port4 ── QCA8084 EPHY @ MDIO 4
 IPQ5018 MAC0 + internal GE PHY (mdio0 addr 7): present in silicon, NOT wired to a socket.
```

- **The QCA8386 is one chip** integrating a switch core + a quad QCA8084 2.5G PHY + 2 PCS + an internal NSS clock controller. Vendor binding, verbatim: *"QCA8084 is quad PHY chip, which integrates 4 PHYs, 2 PCS interfaces (PCS0 and PCS1) and clock controller, which can also be integrated to the switch chip named as QCA8386"* (`qualcommbe/patches-6.18/0301-...-QCA8084-PHY-packag.patch`). So the 4 EPHYs and their clocks live *inside* the QCA8386.
- **Switch/PHY management is on `mdio1`** (SoC gpio36/gpio37, `mdio1_pins` in `qualcommax/dts/ipq5018-mx-base.dtsi:136-150`). Switch register window answers at pseudo‑PHY MDIO **0x10–0x1f**; the 4 EPHYs at **1–4**.
- **EPHY address fix‑up.** Cold‑boot straps put EPHYs at 0,1,2,3; vendor U‑Boot writes `EPHY_CFG` low‑20‑bits = `0x20c41` (four 5‑bit fields = 1,2,3,4). In‑use bitmap `0xffff001e` = bits 1–4 (EPHYs) + 16–31 (switch window). Mainline QCA8084 package driver does this via `qcom,phy-addr-fixup`.
- **Package reset:** `tlmm` **gpio24, active‑low** (`reset-delay-us=10000`, `reset-post-delay-us=50000`).
- **EPHY PHY ID = `0x004dd180`** (QCA8084), confirmed live.
- **CPU link forced 2.5G/full, SGMII+** (`MAC_MODE_SGMII_PLUS`), no in‑band AN.

---

## 1. DSA driver structure — what `qca8k` gives us, what QCA8386 needs new

### 1.1 The existing `qca8k` driver (mainline v6.18)
Files: `drivers/net/dsa/qca/qca8k-8xxx.c` (MDIO front‑end + probe + phylink), `qca8k-common.c` (register I/O, switch ops), `qca8k-leds.c`, `qca8k.h`. OpenWrt layers patches on top (in‑tree):
- `target/linux/generic/backport-6.18/751-v7.2-net-dsa-qca8k-add-support-for-force-mode-for-fixed-l.patch` — force‑mode for fixed links (relevant to the forced‑2.5G CPU port).
- `generic/pending-6.18/711-01..08-*`, `712-*` — LAG, multi‑CPU, PHY‑to‑PHY CPU link, host‑FDB, assisted learning.

`dsa_switch_ops` (`qca8k-8xxx.c:2011-2046`), `phylink_mac_ops` (`:2004-2009`), probe flow `qca8k_sw_probe` (`:2048-2112`), `qca8k_setup` (`:1832-2002`), `phylink_get_caps` (`:1400-1433`, caps top out at **1000FD** — QCA8386 needs **2500FD**). It is an `mdio_driver` (`mdio_module_driver(qca8kmdio_driver)`, `:2211-2222`).

### 1.2 Reusable vs new
Register core is "register‑compatible‑ish" — confirmed: `MHT_TRUNK_HASH_MODE_OFFSET 0x0270` (`ssdk .../include/hsl/mht/mht_reg.h:832`) == `QCA8K_TRUNK_HASH_EN_CTRL 0x270` (`qca8k.h:179`); `MASK_CTRL` at offset 0 (chip ID) is the same. So switch‑core programming (ATU/VTU/FDB/VLAN/port‑lookup/forwarding/MIB) in `qca8k-common.c` is largely portable.

| Area | qca8k | QCA8386 | Action |
|---|---|---|---|
| Chip ID | 0x12/0x13 | **0x17** | new match_data (§3) |
| Indirect‑MDIO page reg | phy `0x18` reg **`0`** (`:151`) | phy `0x18` reg **`0x0c`** | new page setter (§2) |
| Ports | 7 (2 CPU), internal PHYs 0‑4 | CPU port0 + 4 user ports; PHYs **external** QCA8084 on same MDIO | new port map; drop internal‑mdio‑master |
| Internal PHY driver | `qca83xx`/built‑in | none — EPHYs via **`qca808x`** (needs QCA8084) | §5 |
| CPU‑port SerDes | switch `SGMII_CTRL 0xe0` (`:1568-1591`) | QCA8386 **internal UNIPHY/PCS** via MMD regs | §4 |
| PHY caps | 1000FD | **2500FD** | widen `phylink_get_caps` |
| Chip internal clocks/reset | n/a | **NSSCC inside chip** | `nsscc-qca8k` (§5) |

**Recommendation:** build a self‑contained new DSA driver in the OpenWrt tree rather than a mainline `qca8k` patch, for velocity and to avoid fighting qca8k's 7‑port/2‑CPU/internal‑PHY abstractions:
```
target/linux/qualcommax/files/drivers/net/dsa/qca/qca8386.c  (+ qca8386.h)
```
Copy register‑I/O plumbing and switch‑core sequences from `qca8k-8xxx.c`/`qca8k-common.c` verbatim where offsets match; change only: the page setter (§2), chip‑ID match (§3), the port map, `phylink_get_caps` (2500FD), the CPU‑port SerDes hooks (§4), and drop the internal‑mdio‑master code (`qca8k_setup_mdio_bus`/`qca8k_mdio_register`, `:946-1061`) since EPHYs are external. Register as an `mdio_driver` like `qca8k` (`:2211-2222`). Long‑term upstreaming would fold this back into `qca8k` as a variant; defer that.

---

## 2. Regmap over the 32‑bit indirect MDIO

### 2.1 How `qca8k` does it (model to copy)
`qca8k_split_addr` (`qca8k-8xxx.c:27-38`):
```c
regaddr >>= 1; *r1   = regaddr & 0x1e;   // 16-bit word offset within pseudo-phy
regaddr >>= 5; *r2   = regaddr & 0x7;    // selects pseudo-phy 0x10|r2
regaddr >>= 3; *page = regaddr & 0x3ff;  // page (high address)
```
`qca8k_set_page` (`:141-161`) writes the page: `bus->write(bus, 0x18, 0, page)`. `qca8k_read_mii`/`write_mii` (`:428-470`) do lo/hi 16‑bit transfers to pseudo‑PHY `0x10 | r2`, reg `r1`/`r1+1` (`qca8k_mii_read32`/`write32`, `:110-139`). Bus‑less regmap `qca8k_regmap_config` (`:568-584`: `reg_bits=16, val_bits=32, reg_stride=4, disable_locking=true, cache_type=REGCACHE_NONE`) routes `.read/.write/.reg_update_bits` (`:503-566`). Matches the vendor family scheme (`ssdk .../src/hsl/shiva/shiva_reg_access.c:60-68`, `isisc_reg_access.c:83-98`: `phy_addr=0x18, phy_reg=0x0` page write).

### 2.2 The one change for QCA8386
Only the **page register offset** changes:
```c
/* qca8386_set_page(): */
ret = bus->write(bus, 0x18, 0x0c, page);   /* was reg 0 on qca8k */
```
Everything else (split_addr, `0x10|r2` pseudo‑PHY, lo/hi word transfers, page cache init to `0xffff` at `:2086`, bus‑less regmap) reused unchanged. Corroborated by vendor: MHT reg access tagged `SSDK_SWITCH_REG_TYPE_QCA8386` vs `_QCA8337` (`ssdk .../include/init/ssdk_plat.h:387-389`, applied in `qca_mii_reg_convert`, `ssdk_plat.c:276-288`).

**Notes:** keep `priv->mdio_cache.page`, force first write with `0xffff` (`:2086`). For bring‑up, **omit the ethernet‑mgmt (`mgmt_conduit`) fast path** (`:163-410`) — MDIO indirect suffices. `MDIO_IPQ4019` already enabled; `mdio0@88000` carries `"qcom,ipq40xx-mdio"` (patch `0715`); switch must be a child of the `mdio1` node.

---

## 3. Chip identification
`qca8k` reads `QCA8K_REG_MASK_CTRL` (offset `0x0`), device‑ID bits `[15:8]`, revision `[7:0]` (`qca8k.h:42-46`), matches `priv->info->id` (`qca8k_read_switch_id`, `qca8k-common.c:1230-1257`).

**QCA8386 device ID = `0x17`** (`ssdk .../include/hsl/mht/mht_reg.h:26`: `#define MHT_DEVICE_ID 0x17`; cf. qca8k `0x12`/`0x13`). Read from same `MASK_CTRL[15:8]`.
```c
#define QCA8386_ID_QCA8386   0x17
static const struct qca8386_match_data qca8386 = {
    .id = QCA8386_ID_QCA8386, .num_ports = 5 /* port0 CPU + 1..4 */, .page_reg = 0x0c, ...
};
static const struct of_device_id qca8386_of_match[] = {
    { .compatible = "qca,qca8386", .data = &qca8386 }, { }
};
```
Reuse `qca8k_read_switch_id` verbatim. Revision matters (vendor `qca_mht_hw_init`, `ssdk .../src/init/ssdk_mht.c:236`, branches on version; QCA8084 driver keys CDT/serdes quirks off it).

---

## 4. CPU‑port uplink (SoC MAC ↔ switch CPU port at SGMII+ 2.5G) — **already solved on the new stack**

### 4.1 SoC side: stmmac/dwmac‑ipq5018 + uniphy PCS
`gmac1` (`qcom,ipq5018-gmac`/`snps,dwmac`) gets its PCS from **`pcs-handle = <&uniphy0>`** (`qualcommax/files/arch/arm64/boot/dts/qcom/ipq5018-ess.dtsi:74`).

The uniphy PCS driver is in‑tree: **`qualcommax/files/drivers/net/pcs/pcs-qca-uniphy.c`** (patch `patches-6.18/0951-net-pcs-add-uniphy-pcs.patch`; `CONFIG_PCS_QCA_UNIPHY=y`). It **already supports 2500BASE‑X (SGMII+) with the correct soft reset**. In `qca_uniphy_pcs_config_mode` (`:459-658`):
- `case PHY_INTERFACE_MODE_2500BASEX:` (`:507-512`) sets `misc2_phy_mode = UNIPHY_MISC2_SGMIIPLUS` and `mode_ctrl = UNIPHY_SGPLUS_MODE | CH0_MODE_MAC | AUTONEG_MODE_ATH` → `MISC2_PHY_MODE(0x218)=0x5` and `MODE_CTRL(0x46c)` bit 11 (`pcs-qca-uniphy.h:22-38`). Exactly the vendor `sgplus_mode = ENABLE` path (`ssdk .../src/adpt/mp/adpt_mp_uniphy.c:229-241`).
- Analog PLL reset (`:550-555`), then **soft reset** `reset_control_assert/deassert(uniphy->rst_soft)` (`:586-590`), then calibration poll `CALIB_4 & CALIBRATION_DONE` (`:595-598`). SYS/RX/TX soft‑reset, **not AHB** — this is the old‑stack root cause, fixed.
- `uniphy_link_up_sgmii` (`:714-778`) sets rx/tx clocks to **312.5 MHz** for 2500BASEX (`:752-755`).
- GCC soft‑reset wired: `patches-6.18/0723-clk-qcom-gcc-ipq5018-fix-uniphy-soft-reset-issue.patch` sets `GCC_UNIPHY_SOFT_RESET = {0x56104, .bitmask = 0x32}` (SYS/RX/TX). Uniphy node `patches-6.18/0842`: `resets = <&gcc GCC_UNIPHY_SOFT_RESET>; reset-names = "soft"; #pcs-cells = <0>; reg = <0x98000 0x800>`.

**Net effect:** set CPU‑port DT `phy-mode = "2500base-x"`; phylink drives `gmac1`→`uniphy0`. **No new SoC‑side code required.**

### 4.2 Switch side: the QCA8386's own internal SerDes
Both ends must agree. qca8k uses switch `SGMII_CTRL 0xe0` (`qca8k_pcs_config`, `:1534-1614`) + force `PORT_STATUS` at `mac_link_up` (`:1445-1487`). QCA8386 instead has an **internal UNIPHY/PCS over MMD registers** (vendor `qca_mht_serdes_addr_get` + `hsl_phy_modify_mmd(..., MHT_UNIPHY_MMD1, ...)`, `ssdk .../src/hsl/mht/mht_interface_ctrl.c:314-475`), mac mode `MAC_MODE_SGMII_PLUS`.

New driver CPU‑port hooks must:
1. `phylink_get_caps` port 0 → `2500BASEX` + `MAC_2500FD`.
2. `mac_config`/switch‑side `pcs_config` → program QCA8386 internal UNIPHY MMD1 to SGMII+ MAC mode + force 2500/full (port from `mht_interface_ctrl.c`).
3. `mac_link_up` → force `PORT_STATUS` port0 to 2500/full/flow (extend `:1445-1487` with the MHT 2500 speed encoding — qca8k's `PORT_STATUS_SPEED` only encodes 10/100/1000; pull MHT 2500 encoding from `mht_reg.h`).
4. Backported **force‑mode‑for‑fixed‑link** (`backport-6.18/751`) is the foundation for a `fixed-link` CPU port.

**Old‑stack swphy caveat now gone:** on the new stack `phy-mode = "2500base-x"` on a fixed CPU port + the uniphy PCS carries the rate natively; no `speed = <1000>` fudge.

---

## 5. QCA8084 EPHYs (MDIO 1–4, ID `0x004dd180`)

### 5.1 Driver status: partially present, must be ported into qualcommax
- Base `qca808x` enabled (`CONFIG_QCA808X_PHY=y`); mainline v6.18 `qca808x` supports **QCA8081** (`0x004dd101`, what spnmx56 uses).
- **QCA8084 (`0x004dd180`) is NOT in qualcommax.** It exists only in **qualcommbe** patches: `qualcommbe/patches-6.18/0301..0312` (binding `0301`; `QCA8084_PHY_ID 0x004dd180` in `0302`; probe/config_init/package‑init/clocks‑resets/serdes in `0303`–`0312`; uniphy 2500BASEX/MISC2 bits `0318`/`0321`).
- QCA8386 internal clocks/resets → mainline **`nsscc-qca8k`** (`drivers/clk/qcom/nsscc-qca8k.c`, present in v6.18; bindings `include/dt-bindings/{clock,reset}/qcom,qca8k-nsscc.h`). **Enabled on qualcommbe/ipq95xx** (`CONFIG_IPQ_NSSCC_QCA8K=y`) but **disabled on qualcommax**.

**Work:** copy `qualcommbe/patches-6.18/0301`–`0312` (+ uniphy 2.5G helpers if not already in `pcs-qca-uniphy.c`) into `qualcommax/patches-6.18/` (renumber into a free range, e.g. `09xx`); enable `CONFIG_IPQ_NSSCC_QCA8K=y` in `qualcommax/ipq50xx/config-default`. `include/dt-bindings/net/qcom,qca808x.h` is created by binding patch `0301` (not in mainline v6.18) — comes across with it.

### 5.2 DT the EPHYs + package need
Described as an `ethernet-phy-package` (like `qcom,qca8075-package` in `ipq8074-rt-ax89x.dts:524-550`). Vendor example (`0301` patch):
```dts
ethernet-phy-package@1 {
    compatible = "qcom,qca8084-package";
    reg = <1>;  #address-cells = <1>; #size-cells = <0>;
    clocks = <&qca8k_nsscc NSS_CC_APB_BRIDGE_CLK>, ... <&qca8k_nsscc NSS_CC_MDIO_AHB_CLK>;
    clock-names = "apb_bridge","ahb","sec_ctrl_ahb","tlmm","tlmm_ahb","cnoc_ahb","mdio_ahb";
    resets = <&qca8k_nsscc NSS_CC_GEPHY_FULL_ARES>;
    qcom,package-mode = <...>;               /* interface mode of the 2 PCSes */
    qcom,phy-addr-fixup = <1 2 3 4 5 6 7>;   /* == vendor 0x20c41 EPHY_CFG write */
    ethernet-phy@1 { compatible = "ethernet-phy-id004d.d180"; reg=<1>;
                     clocks=<&qca8k_nsscc NSS_CC_GEPHY0_SYS_CLK>;
                     resets=<&qca8k_nsscc NSS_CC_GEPHY0_SYS_ARES>; qcom,xpcs-channel=<0>; };
    /* @2..@4 likewise */
    pcs-phy@6 { compatible="qcom,qca8k-pcs-phy"; ... };
    xpcs-phy@7 { compatible="qcom,qca8k-xpcs-phy"; ... };
};
```
Consequences:
- **`qcom,phy-addr-fixup` replaces the Phase‑1 custom preinit.** Do **not** carry `0919-...-QCA8084-preinit...patch` on the new stack — the mainline package driver does the `0x20c41` address fix‑up itself.
- **`nsscc` is mandatory** to clock the EPHYs; without it, "phy_addr N phydev is NULL"‑style failures reappear.

---

## 6. Device tree for `spnmx57` on the new stack
Model on `ipq5018-spnmx56.dts` (qca8k on ipq50xx new stack) + `ipq8074-rt-ax89x.dts:552-620` (CPU `port@0` + package pattern) + the Phase‑1 old‑stack `spnmx57.dts` (authoritative topology).

```dts
&uniphy0 { status = "okay"; };
&gmac1 {
    status = "okay";                          /* pcs-handle=<&uniphy0> from ess.dtsi:74 */
    nvmem-cells = <&hw_mac_addr 0>;
    nvmem-cell-names = "mac-address";
};
&mdio0 { status = "okay"; };                   /* required for SoC MDIO platform device */
&mdio1 {
    status = "okay";
    pinctrl-0 = <&mdio1_pins>; pinctrl-names = "default";
    reset-gpios = <&tlmm 24 GPIO_ACTIVE_LOW>;  /* QCA8386 package reset, per RE */
    reset-delay-us = <10000>;
    reset-post-delay-us = <50000>;

    qca8k_nsscc: clock-controller@18 {         /* address TBD from binding + live scan */
        compatible = "qcom,qca8k-nsscc";
        #clock-cells = <1>; #reset-cells = <1>;
    };
    ephy_package: ethernet-phy-package@1 {
        compatible = "qcom,qca8084-package"; reg = <1>;
        #address-cells = <1>; #size-cells = <0>;
        clocks = <&qca8k_nsscc ...>; clock-names = "apb_bridge", ...;
        resets = <&qca8k_nsscc NSS_CC_GEPHY_FULL_ARES>;
        qcom,phy-addr-fixup = <1 2 3 4 5 6 7>;
        qcom,package-mode = <...>;
        ephy1: ethernet-phy@1 { compatible="ethernet-phy-id004d.d180"; reg=<1>; qcom,xpcs-channel=<0>; };
        ephy2: ethernet-phy@2 { compatible="ethernet-phy-id004d.d180"; reg=<2>; qcom,xpcs-channel=<1>; };
        ephy3: ethernet-phy@3 { compatible="ethernet-phy-id004d.d180"; reg=<3>; qcom,xpcs-channel=<2>; };
        ephy4: ethernet-phy@4 { compatible="ethernet-phy-id004d.d180"; reg=<4>; qcom,xpcs-channel=<3>; };
    };
    switch0: ethernet-switch@10 {
        compatible = "qca,qca8386"; reg = <0x10>;
        #address-cells = <1>; #size-cells = <0>;
        ports {
            #address-cells = <1>; #size-cells = <0>;
            port@0 { reg=<0>; label="cpu"; ethernet=<&gmac1>;
                     phy-mode="2500base-x"; fixed-link { speed=<2500>; full-duplex; }; };
            port@1 { reg=<1>; label="lan1"; phy-handle=<&ephy1>; phy-mode="2500base-x"; };
            port@2 { reg=<2>; label="lan2"; phy-handle=<&ephy2>; phy-mode="2500base-x"; };
            port@3 { reg=<3>; label="lan3"; phy-handle=<&ephy3>; phy-mode="2500base-x"; };
            port@4 { reg=<4>; label="lan4"; phy-handle=<&ephy4>; phy-mode="2500base-x"; };
        };
    };
};
```
DT notes:
- `mdio0` **must** be `okay` even though MAC0 is unused (SoC MDIO platform device/bus lookup). Keep `ge_phy` disabled.
- On the old stack the reset GPIO was renamed `qca8084-reset-gpios` (preinit ran before `__mdiobus_register()`). On the new stack, use standard `reset-gpios` — **verify which node the `qca8084-package` binding expects the reset on**, so gpio24 is claimed exactly once.
- CPU tag protocol: start with **`DSA_TAG_PROTO_QCA`** (in‑band Atheros header, `PORT_HDR_CTRL`, `:1905-1913`). `NET_DSA_TAG_OOB` is available (`CONFIG_NET_DSA_TAG_OOB=y`) as fallback.
- Single switch → `dsa,member = <0 0>`.

---

## 7. Phased task list

### Phase 2a — dependencies in place (no traffic yet)
1. **Port QCA8084 PHY + package** from qualcommbe→qualcommax: copy `qualcommbe/patches-6.18/0301`–`0312` → `qualcommax/patches-6.18/` (renumber `09xx`).
2. **Enable NSS clock controller:** add `CONFIG_IPQ_NSSCC_QCA8K=y` to `qualcommax/ipq50xx/config-default`.
3. Build+boot current `ipq5018-spnmx57.dts` (ethernet still disabled) to confirm wifi‑only boots on the new toolchain with the new configs. **← this is the Phase‑1 build in progress.**

### Phase 2b — DSA driver skeleton + register I/O
4. Create `qualcommax/files/drivers/net/dsa/qca/qca8386.{c,h}` + Kconfig/Makefile (`CONFIG_NET_DSA_QCA8386`), add to `config-default`. Copy from qca8k: indirect‑MDIO front‑end (page write → `0x18,0x0c`), bus‑less regmap (MDIO‑only, drop `mgmt_conduit`), `read_switch_id` matching `0x17`, probe as `mdio_driver`.
5. Add DT switch/nsscc/package nodes (§6); set `&mdio0/&mdio1/&uniphy0/&gmac1` okay. **Success = driver probes, `read_switch_id` returns `0x17`.**

### Phase 2c — CPU port passes traffic
6. Implement `qca8386_setup` (port `qca8k_setup` `:1832-2002`, drop internal‑mdio‑master `:946-1061`; keep CPU‑port‑enable, QCA header mode, unknown‑frame flood to CPU, per‑port lookup/forwarding). Verify MHT offsets against `mht_reg.h`.
7. CPU‑port phylink (§4.2): caps 2500FD, program internal UNIPHY MMD1 SGMII+, force `PORT_STATUS` 2500/full. SoC side needs no new code. **Success = DSA conduit + `lan*` netdevs, ping traverses CPU↔a forwarded port.**
8. Tag protocol `DSA_TAG_PROTO_QCA`; verify over stmmac; fall back to `OOB` if needed.

### Phase 2d — 4 user ports at 2.5G
9. Wire ports 1–4 to `&ephy1..4`; confirm package driver does addr fix‑up + NSSCC clocks them.
10. Per‑port caps 2500FD; `mac_link_up` force/inband. Confirm negotiated 2.5G per port.
11. Bridge `lan*` under `br-lan`; verify L2 forwarding, VLAN filtering, FDB. **Success = 4 ports at 2.5G passing bridged traffic.**

### Files to create / patch (summary)
- **Create:** `qualcommax/files/drivers/net/dsa/qca/qca8386.{c,h}` (+ Kconfig/Makefile glue).
- **Create/replace:** `qualcommax/dts/ipq5018-spnmx57.dts` (enable ethernet per §6).
- **Copy in:** `qualcommax/patches-6.18/09xx-*` ← qualcommbe `0301`–`0312` [+ `0318/0321/0322` if uniphy 2.5G not already covered].
- **Edit:** `qualcommax/ipq50xx/config-default` — add `CONFIG_NET_DSA_QCA8386=y`, `CONFIG_IPQ_NSSCC_QCA8K=y` (confirm `QCA808X_PHY`, `PCS_QCA_UNIPHY`, `NET_DSA`, `NET_DSA_TAG_QCA`, `STMMAC_ETH` — all already present).
- **Reference only (do not vendor):** `ssdk` MHT reg offsets + internal‑UNIPHY MMD sequence; old‑stack DTS for topology/GPIO. **Do not** port the old‑stack `0919` preinit patch.

---

## Open items to nail down during implementation
1. **Exact MHT register offsets** for `PORT_STATUS` (incl. 2500 speed encoding), `PORT_HDR_CTRL`, `GLOBAL_FW_CTRL0/1`, `PORT_LOOKUP_CTRL`, port‑bitmask width — from `mht_reg.h`.
2. **QCA8386 internal‑UNIPHY MMD sequence** for CPU‑port SerDes — port from `mht_interface_ctrl.c`; decide switch‑internal `phylink_pcs` vs inline `mac_config`.
3. **`nsscc` + `qca8084-package` DT reg/clock topology** on *this* board (vendor example targets standalone ipq95xx) — validate against a live `mdio1` scan.
4. **Which node owns gpio24 reset** under the package binding — claimed exactly once.
5. **`num_ports`** for `dsa_register_switch` — CPU + 4 (5); confirm no 6th SerDes/WAN port (old‑stack `switch_wan_bmp = <0>`).
