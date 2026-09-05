## SPNMX57 device-tree: QCA8386 NSS clock-controller (nsscc-qca8k) + QCA8084 PHY package

All nodes go **under `&mdio1`** (the QCA8386 bus; old-stack `ipq5018-spnmx57.dts:49` `mdio-bus = <&mdio1>`, `:232` `&mdio1 { status = "okay" }`). Every value below is cited; values that can only be confirmed with a live MDIO scan on the board are tagged **[VALIDATE]**.

Add these three includes to the board .dts:
```
#include <dt-bindings/clock/qcom,qca8k-nsscc.h>
#include <dt-bindings/reset/qcom,qca8k-nsscc.h>
#include <dt-bindings/net/qcom,qca808x.h>
```
(Same set the binding example uses — `0930-*.patch` example header, and the driver includes both nsscc dt-binding headers at `srv-openstack:/tmp/nsscc-qca8k.c:16-17`.)

---

### (1) NSS clock & reset controller node (`nsscc-qca8k`)

```dts
&mdio1 {
    #address-cells = <1>;
    #size-cells = <0>;

    qca8k_nsscc: clock-controller@18 {
        /* QCA8386 = switch variant of QCA8084; two-item compatible so the
         * qca8386 id is precise and the qca8084 fallback is what binds. */
        compatible = "qcom,qca8386-nsscc", "qcom,qca8084-nsscc";
        reg = <0x18>;

        /* THIS node, and only this node, owns the gpio24 package reset. */
        reset-gpios = <&tlmm 24 GPIO_ACTIVE_LOW>;

        /* 7 parents, POSITIONAL (no clock-names). Order fixed by driver enum
         * DT_XO..DT_UNIPHY1_TX312P5M_CLK. */
        clocks = <&qca8386_ref50m>,        /* DT_XO  : 50 MHz chip ref  [VALIDATE] */
                 <&qca8k_uniphy0_rx>,      /* DT_UNIPHY0_RX_CLK   [NO in-tree provider] */
                 <&qca8k_uniphy0_tx>,      /* DT_UNIPHY0_TX_CLK   [NO in-tree provider] */
                 <&qca8k_uniphy1_rx>,      /* DT_UNIPHY1_RX_CLK   [NO in-tree provider] */
                 <&qca8k_uniphy1_tx>,      /* DT_UNIPHY1_TX_CLK   [NO in-tree provider] */
                 <&qca8k_uniphy1_rx312p5m>,/* DT_UNIPHY1_RX312P5M_CLK [NO in-tree provider] */
                 <&qca8k_uniphy1_tx312p5m>;/* DT_UNIPHY1_TX312P5M_CLK [NO in-tree provider] */

        #clock-cells = <1>;
        #reset-cells = <1>;
        #power-domain-cells = <1>;
    };
};
```

**compatible** — `qcom,qca8084-nsscc` is the only string the driver matches (`/tmp/nsscc-qca8k.c:2204-2206` `nss_cc_qca8k_match_table[] = { .compatible = "qcom,qca8084-nsscc" }`). Because this board carries the switch-integrated QCA8386, the binding's two-item form `qcom,qca8386-nsscc`,`qcom,qca8084-nsscc` is the correct precise value (binding `compatible.oneOf`, `qcom,qca8k-nsscc.yaml` @v6.18 — `qca8386-nsscc` is one of the listed prefix enums with `const: qcom,qca8084-nsscc`). Bare `qcom,qca8084-nsscc` also works.

**reg = `<0x18>`** — The driver is an `mdio_driver` (`/tmp/nsscc-qca8k.c:2208` `mdio_module_driver`); the node is an MDIO child. The register access **ignores `mdiodev->addr`**: regmap context is `mdiodev->bus` and every access hardcodes the window — page written to phy `0x18` reg `0xc` (`:28` `QCA8K_HIGH_ADDR_PREFIX 0x18`, `:30` `QCA8K_CFG_PAGE_REG 0xc`, `:2084` `qca8k_mii_page_set(bus, QCA8K_HIGH_ADDR_PREFIX, QCA8K_CFG_PAGE_REG, page)`), data at phy `0x10 | regbits[7:5]` (`:29` `QCA8K_LOW_ADDR_PREFIX 0x10`, `:2027`). This is exactly the RE'd "window at pseudo-PHY 0x10-0x1f, page to phy 0x18 reg 0x0c." `0x18` matches the mainline binding example (`clock-controller@18 { reg = <0x18> }`, `qcom,qca8k-nsscc.yaml` example) and sits inside the 0x10-0x1f switch-register block the RE confirmed is reserved (old-stack DTS `ipq5018-spnmx57.dts` EPHY-address commentary), so it will not collide with the EPHYs (1-4) or PCS (5-7). **[VALIDATE]** that nothing else on the live bus answers at 0x18, but any address in 0x10-0x1f is functionally equivalent since the driver hardcodes the window.

**reset-gpios = `<&tlmm 24 GPIO_ACTIVE_LOW>`** — GPIO24, active-low, from the authoritative old-stack value (`ipq5018-spnmx57.dts:272` `qca8084-reset-gpios = <&tlmm 24 GPIO_ACTIVE_LOW>`; the vendor line is `<&tlmm 0x18 0x00>` = gpio24, and RE chose ACTIVE_LOW because the vendor pulses low-then-high). The nsscc driver claims property **`reset-gpios`** (`/tmp/nsscc-qca8k.c:2177` `devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH)`) and pulses it: assert (100 ms), then `gpiod_set_value_cansleep(gpiod, 0)` deassert (`:2170-2183`) — matching the low-then-high behaviour. **[VALIDATE]** polarity on hardware: if the package never leaves reset, try `GPIO_ACTIVE_HIGH` (the vendor flag cell is 0; old-stack DTS calls this a guess at `:262-269`).

**clocks (7, positional, NO `clock-names`)** — Required by binding (`qcom,qca8k-nsscc.yaml` `required: [compatible, clocks, reg, reset-gpios]`); items in order are: chip reference; UNIPHY0 RX; UNIPHY0 TX; UNIPHY1 RX; UNIPHY1 TX; UNIPHY1 RX312P5M; UNIPHY1 TX312P5M. Driver consumes them by `.index` (`/tmp/nsscc-qca8k.c:36-44` enum `DT_XO..DT_UNIPHY1_TX312P5M_CLK`, `:60-63` `clk_parent_data { .index = DT_XO }`), i.e. **positional — do not add `clock-names`.**
  - **[VALIDATE — hard gap]** There is **no in-tree provider** for the six `qca8k_uniphy*` clocks anywhere in the merged v6.18 set: the OpenWrt serdes patches (`0939/0940/0941-*.patch`) only add PCS/XPCS *probe* called from the package (`0939-*.patch:370-386`), they register no `qca8k_uniphy*` clock; no board .dts in v6.18 instantiates `qca8084-nsscc` (checked `ipq5424-rdp466.dts`/`ipq5424.dtsi` @v6.18 — no reference), and there is no qca8k PCS clock-provider driver in v6.18 `drivers/net/pcs/`. On this board those six clocks are generated inside the QCA8386's PCS0/PCS1, which in the current design are driven by qca-ssdk, not by a mainline DT clock provider. To make the mainline nsscc probe you must either supply `fixed-clock` stubs (UNIPHY1 tx/rx312p5m = 312500000, the /125M lines = 125000000) or add a PCS clock-provider — and confirm the resulting link clocking on hardware. `&qca8386_ref50m` (the DT_XO input) likewise needs a real source: the QCA8386's 50 MHz reference (binding description: "QCA8084 expects an input reference clock 50 MHZ"); model it as a `fixed-clock` and confirm against the board schematic.

**#clock-cells `<1>` / #reset-cells `<1>` / #power-domain-cells `<1>`** — from the binding example (`qcom,qca8k-nsscc.yaml` example: all three present); driver registers clocks+resets via `qcom_cc_really_probe` (`/tmp/nsscc-qca8k.c:2201`).

---

### (2) QCA8084 PHY-package node

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

        /* DEFAULT shown = 10G-QXGMII. SPNMX57 uplink is SGMII+ 2.5G (switch
         * mode) => this is almost certainly wrong; see [VALIDATE] below. */
        qcom,package-mode = <QCA808X_PCS1_10G_QXGMII_PCS0_UNUNSED>;
        qcom,phy-addr-fixup = <1 2 3 4 5 6 7>;

        ethernet-phy@1 {
            compatible = "ethernet-phy-id004d.d180";
            reg = <1>;
            clocks = <&qca8k_nsscc NSS_CC_GEPHY0_SYS_CLK>;
            resets = <&qca8k_nsscc NSS_CC_GEPHY0_SYS_ARES>;
            qcom,xpcs-channel = <0>;      /* [VALIDATE] only meaningful in QXGMII */
        };
        ethernet-phy@2 {
            compatible = "ethernet-phy-id004d.d180";
            reg = <2>;
            clocks = <&qca8k_nsscc NSS_CC_GEPHY1_SYS_CLK>;
            resets = <&qca8k_nsscc NSS_CC_GEPHY1_SYS_ARES>;
            qcom,xpcs-channel = <1>;
        };
        ethernet-phy@3 {
            compatible = "ethernet-phy-id004d.d180";
            reg = <3>;
            clocks = <&qca8k_nsscc NSS_CC_GEPHY2_SYS_CLK>;
            resets = <&qca8k_nsscc NSS_CC_GEPHY2_SYS_ARES>;
            qcom,xpcs-channel = <2>;
        };
        ethernet-phy@4 {
            compatible = "ethernet-phy-id004d.d180";
            reg = <4>;
            clocks = <&qca8k_nsscc NSS_CC_GEPHY3_SYS_CLK>;
            resets = <&qca8k_nsscc NSS_CC_GEPHY3_SYS_ARES>;
            qcom,xpcs-channel = <3>;
        };

        /* Needed ONLY for 10G-QXGMII (package-mode 0); probed by
         * qca8084_package_pcs_probe/xpcs_probe from the package node
         * (0939-*.patch:370-386). Their pcs_rx_root/pcs_tx_root are the same
         * unprovided uniphy clocks flagged above. Omit in SGMII+ switch mode. */
        /* pcs-phy@6 { ... }  xpcs-phy@7 { channel@0..3 } */
    };
};
```

**compatible = `qcom,qca8084-package`** — `0930-*.patch:78`, example `:319-320`.

**reg = `<1>`** — package base MDIO address = first EPHY. `0930-*.patch:322`. SPNMX57's four EPHYs answer at MDIO **1,2,3,4** (old-stack DTS `ipq5018-spnmx57.dts:290-303` `ethernet-phy@1..@4 reg=<0x01..0x04>`, confirmed live reporting PHY ID `0x004dd180`), so base `1` is correct and consistent with the RE'd `0x20c41` EPHY_CFG write.

**clocks + clock-names (7 package clocks)** — exact list and names from `0930-*.patch:88-104` (clock-names `:96` `apb_bridge` … `mdio_ahb`) and example `:323-336`. IDs from the clock header (`qcom,qca8k-nsscc.h`): `NSS_CC_APB_BRIDGE_CLK=2`, `NSS_CC_AHB_CLK=78`, `NSS_CC_SEC_CTRL_AHB_CLK=79`, `NSS_CC_TLMM_CLK=80`, `NSS_CC_TLMM_AHB_CLK=81`, `NSS_CC_CNOC_AHB_CLK=82`, `NSS_CC_MDIO_AHB_CLK=83`. Driver reads them by name (`0936-*.patch` `qca8084_package_clk_name[]` = `apb_bridge`/`ahb`/`sec_ctrl_ahb`/`tlmm`/`tlmm_ahb`/`cnoc_ahb`/`mdio_ahb`, plus optional `mdio_master_ahb`/`switch_core` which are NOT provided here — code treats `-EINVAL` as "clock=NULL, skip", so omitting them is fine).

**resets = `<&qca8k_nsscc NSS_CC_GEPHY_FULL_ARES>`** — single reset, `maxItems:1` (`0930-*.patch:105-111`, example `:337`). `NSS_CC_GEPHY_FULL_ARES = 65` (reset header `qcom,qca8k-nsscc.h`). This is the package DSP reset **provided by the nsscc over MDIO** — it is *not* the GPIO (see section 3). Deasserted at `0936-*.patch` `reset_control_deassert(rstc)`.

**qcom,phy-addr-fixup = `<1 2 3 4 5 6 7>`** — 7 values = [PHY0,PHY1,PHY2,PHY3, PCS0, PCS1-PCS, PCS1-XPCS] (`0930-*.patch:133-140`, example `:339`). First four (1,2,3,4) are **confirmed** for SPNMX57 (EPHYs at 1-4, as above) and happen to equal the binding example. **[VALIDATE]** the last three (PCS0=5, PCS1-PCS=6, PCS1-XPCS=7): the RE only proved EPHYs 1-4 and the 0x10-0x1f window; 5/6/7 are the binding defaults and sit safely below the window, but confirm with a live scan (and note this fixup is a *different* register path from qca-ssdk's EPHY_CFG, so its effect must be verified on hardware).

**qcom,package-mode** — `0930-*.patch:112-131`, `enum [0,1,2]`, default 0. Values from `qcom,qca808x.h` (`0930-*.patch` second file): `QCA808X_PCS1_10G_QXGMII_PCS0_UNUNSED=0`, `QCA808X_PCS1_SGMII_MAC_PCS0_SGMII_MAC=1`, `QCA808X_PCS1_SGMII_MAC_PCS0_SGMII_PHY=2`.
  - **[VALIDATE — likely change to `<1>`]** SPNMX57's uplink is **SGMII+ 2.5G, single link** (`ipq5018-spnmx57.dts:53/117` `switch_mac_mode = <MAC_MODE_SGMII_PLUS>`, CPU port `forced-speed = <2500>` `:66/172`), i.e. **switch mode**, not 10G-QXGMII. That points to `qcom,package-mode = <QCA808X_PCS1_SGMII_MAC_PCS0_SGMII_MAC>` (=1). I show the binding-example default (0) verbatim per the task, but flag that the board evidence contradicts it.

**qcom,xpcs-channel on the four children** — `0930-*.patch:141-149` + example `:346/354/362/370`; per-PHY IDs 0/1/2/3. Per-child `clocks`/`resets` are `NSS_CC_GEPHY{0..3}_SYS_CLK` (header: 88/89/90/91) and `NSS_CC_GEPHY{0..3}_SYS_ARES` (reset header: 52/53/54/55). **[VALIDATE]** `qcom,xpcs-channel` is documented as meaningful **only when PCS1 runs 10G-QXGMII** (`0930-*.patch:142-148`). If `package-mode = <1>` (SGMII+ switch mode, the board's real config) these properties are inert and the `xpcs-phy@7` channel subtree is not used; keep them only if you actually run QXGMII.

**Child `compatible = "ethernet-phy-id004d.d180"`** — matches the live-read PHY ID `0x004dd180` (old-stack DTS `:288`; binding `0930-*.patch:150`).

---

### (3) Ownership of the gpio24 package reset — UNAMBIGUOUS

**The `clock-controller@18` (nsscc) node owns gpio24, and it is the ONLY node that may carry `reset-gpios`.**

Rationale, from the drivers:
- The nsscc driver is the one that claims and pulses the line: `devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH)` → assert → `msleep(100)` → deassert (`/tmp/nsscc-qca8k.c:2170-2183`), run *before* it can talk MDIO to bring up the package clocks (`:2188-2201`). The binding makes `reset-gpios` **required** on this node (`qcom,qca8k-nsscc.yaml` `required`).
- The `ethernet-phy-package@1` node must **NOT** have `reset-gpios`: its binding has no such property; it resets the package via the CC instead — `resets = <&qca8k_nsscc NSS_CC_GEPHY_FULL_ARES>` (`0930-*.patch:105-111`, `0936-*.patch` `of_reset_control_get_exclusive` + `reset_control_deassert`). A GPIO here would be a second, conflicting claimant.
- The old-stack private property **`qca8084-reset-gpios` on `&mdio1` must be deleted** (`ipq5018-spnmx57.dts:272`). It exists only for the old qca-ssdk preinit path; leaving it would double-drive gpio24. Likewise drop the old-stack `qcom,qca8084-preinit*`, `reset-delay-us`, `reset-post-delay-us` (`:250-274`) and the bare `ethernet-phy@1..4` stubs (`:290-303`) — the package node replaces them.
- Do **not** use the standard MDIO `reset-gpios` on the `&mdio1` bus node either: `__mdiobus_register()` would claim and pulse it independently of the nsscc, a third conflicting claimant.

So: exactly one `reset-gpios = <&tlmm 24 GPIO_ACTIVE_LOW>`, on `clock-controller@18`.

---

### Source list (all read directly)
- Package binding + full DT example + `qcom,qca808x.h`: `srv-openstack:/tmp/owrt-main/target/linux/qualcommax/patches-6.18/0930-dt-bindings-net-Document-Qualcomm-QCA8084-PHY-packag.patch`
- Package clocks/resets consumer code: `.../patches-6.18/0936-net-phy-qca808x-Add-package-clocks-and-resets-for-QC.patch`
- PCS/XPCS probe wiring: `.../patches-6.18/0939-net-phy-qca808x-Add-QCA8084-SerDes-probe-and-remove-.patch`
- nsscc driver (fetched v6.18, saved): `srv-openstack:/tmp/nsscc-qca8k.c` (mirror of `drivers/clk/qcom/nsscc-qca8k.c` @v6.18)
- nsscc clock-controller binding (fetched v6.18): `Documentation/devicetree/bindings/clock/qcom,qca8k-nsscc.yaml`
- Clock IDs: `srv-openstack:/tmp/owrt-main/build_dir/target-aarch64_cortex-a53_musl/linux-qualcommax_ipq50xx/linux-6.18.44/include/dt-bindings/clock/qcom,qca8k-nsscc.h` (identical to v6.18 kernel.org copy I fetched)
- Reset IDs: same path, `.../reset/qcom,qca8k-nsscc.h`
- Board topology/GPIO/mdio: `srv-openstack:/home/luca/spnmx57/openwrt/target/linux/qualcommax/files/arch/arm64/boot/dts/qcom/ipq5018-spnmx57.dts`
(WebFetch to git.kernel.org returned HTTP 403; I fetched the same raw files with `curl` from `srv-openstack`, which succeeded.)