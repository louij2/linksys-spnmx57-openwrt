Vendor device tree for the SPNMX57, extracted from the stock image, plus a
corroboration of the -22 root cause.

Short version: the OEM image is a u-boot FIT, which is itself a flattened
device tree, so the vendor DTB comes straight out with dtc. No hardware, no
GPL dump needed. It confirms Hyndland's analysis and it corrects a couple of
things the thread has been assuming.

---

**Getting the vendor DTB**

    curl -fLO http://download.linksys.com/updates/20250403t130837/FW_MX57CF_1.0.1.216553_prod.img
    # md5 62b76e25b194ecd42275460a7eedcace

`file` reports it as a Device Tree Blob, because it is a FIT image. `dtc -I dtb
-O dts` on it gives the structure:

    kernel@1   ARM OpenWrt Linux-5.4.213, gzip, load/entry 0x41208000
    fdt@1      ARM OpenWrt Palm15 device tree blob, compression = "none"

`Palm15` is the vendor board codename. Because `fdt@1` is uncompressed, the
inner blob is just the second `d00dfeed` in the file, at offset 0x499c30,
totalsize 43034. Copy that range out and decompile it.

The tree self identifies as `Qualcomm Technologies, Inc. IPQ5018/AP-MP03.1`,
so Linksys adapted the reference DTS. Some nodes are reference board
boilerplate; the Ethernet nodes are clearly board specific.

---

**What the board actually is**

                    IPQ5018
      +----------------------------------+
      |  MAC0 -- internal GE PHY         |   mdio@88000 addr 7, real silicon,
      |                                  |   but NO nss-dp node. Not wired to
      |                                  |   any socket on this board
      |                                  |
      |  MAC1 -- UNIPHY1 ----------------+-- SGMII+ 2.5G forced --+
      |          (dp1, qcom,id = 2)      |                        |
      +----------------------------------+                        v
                                               +----------------------------+
                                               |          QCA8386           |
                                               |  port0 = CPU, 2500/full    |
                                               |  port1..4 = QCA8084 EPHYs  |
                                               |             at addr 1,2,3,4|
                                               +----------------------------+
                                                    on mdio@90000

Three corrections to what has been assumed in this thread:

1. **The EPHYs are at MDIO addresses 1, 2, 3, 4.** Not 0, and not 0x1c. 0x1c
   was the SPNMX56's QCA8081, which does not exist on this board.
2. **There is exactly one nss-dp node in the whole vendor tree.** `dp1`,
   `qcom,id = <2>` (MAC1), `qcom,mactype = <2>` (Synopsys XGMAC),
   `phy-mode = "sgmii"`. MAC0 has none. The `port_id = 1 / phy_address = 7`
   entry on the SoC side switch node is AP-MP03.1 boilerplate.
3. **The uplink is SGMII+, not 10G-QXGMII.** `switch_mac_mode = <0x0c>`, which
   is `MAC_MODE_SGMII_PLUS` per `dt-bindings/net/qcom-ipq-ess.h`, with
   `forced-speed = <2500>` on both ends. `switch_mac_mode1 = <0xff>`
   (`MAC_MODE_DISABLED`), so the second uplink is unused.

The relevant nodes verbatim:

    mdio@90000 {
            compatible       = "qcom,qca-mdio";
            reg              = <0x90000 0x64>;
            pinctrl-0        = <&mdio_pinmux>;   /* mdc gpio36, mdio gpio37 */
            phy-reset-gpio   = <&tlmm 0x18 0x00>;    /* gpio24 */
            phyaddr_fixup    = <0xc90f018>;
            uniphyaddr_fixup = <0xc90f014>;
            mdio_clk_fixup;

            ethernet-phy@0 { reg = <0x01>; fixup; };
            ethernet-phy@1 { reg = <0x02>; fixup; };
            ethernet-phy@2 { reg = <0x03>; fixup; };
            ethernet-phy@3 { reg = <0x04>; fixup; };
    };

    ess-switch1@1 {
            compatible         = "qcom,ess-switch-qca8386";
            switch_access_mode = "mdio";
            mdio-bus           = <&mdio1>;
            switch_mac_mode    = <0x0c>;   /* SGMII_PLUS */
            switch_mac_mode1   = <0xff>;   /* DISABLED */
            switch_cpu_bmp     = <0x01>;   /* port 0 */
            switch_lan_bmp     = <0x1e>;   /* ports 1,2,3,4 */
            switch_wan_bmp     = <0x00>;   /* none */

            port@0 { port_id = <0>; forced-speed = <2500>; forced-duplex = <1>; };
            port@1 { port_id = <1>; phy_address = <1>; };
            port@2 { port_id = <2>; phy_address = <2>; };
            port@3 { port_id = <3>; phy_address = <3>; };
            port@4 { port_id = <4>; phy_address = <4>; };
    };

Note `switch_wan_bmp = 0`. There is no hardware WAN port; all four sockets are
LAN on the switch and the WAN split is done in software by the stock firmware.

---

**This corroborates Hyndland's root cause**

The `phyaddr_fixup = <0xc90f018>` and `uniphyaddr_fixup = <0xc90f014>` in the
vendor tree are exactly the strap registers named in that analysis, and
`mdio_clk_fixup` plus the `fixup` flag on each PHY say the same thing in the
vendor's vocabulary: the chip does not answer at 1..4 until it is clocked and
then strapped there.

Which also explains yomod83706's scan reading `0x00000000` at every address on
`mdio@90000` while `mdio@88000` addr 7 answered `0x004dd0c0`. That is a dead,
unclocked bus rather than a wrong address, and it is the clock parent liveness
bug visible from userspace.

So the DT side needs the `ethernet-phy-package` container with
`compatible = "qcom,qca8084-package"`, the `qca8084-nsscc` clock controller,
`CONFIG_IPQ_NSSCC_QCA8K=y`, and `qcom,phy-addr-fixup` with the EPHYs at 1,2,3,4.

One mismatch worth raising. `qcom,package-mode` offers:

    0  QCA808X_PCS1_10G_QXGMII_PCS0_UNUNSED
    1  QCA808X_PCS1_SGMII_MAC_PCS0_SGMII_MAC
    2  QCA808X_PCS1_SGMII_MAC_PCS0_SGMII_PHY

The vendor configuration is PCS1 in SGMII+ with PCS0 disabled, which is none of
those three. Mode 1 looks closest since PCS0 is unused either way, but I would
value a second opinion from anyone who knows the PCS side better.

---

**The part that worries me more than the -22**

There is no mainline driver for the QCA8386. `qca8k` covers the QCA8327 and
QCA8337; the vendor drives this one with `qcom,ess-switch-qca8386` out of the
SSDK.

So as far as I can see, fixing the `-22` gets the package clocked, strapped and
probing, and Linux ends up with **one** interface with four sockets behind it.
Whether any traffic actually moves then depends on whether the QCA8386 comes
out of reset with all ports forwarding, which Hyndland flagged as the thing
they could not verify. If it does not, this stops being a device tree fix.

---

**Offer**

I have UART headers going onto one of these and a second unit still on stock,
so I can run the serial confirm Hyndland asked for: reading `mdio@90000`
addresses 1 to 4 and reporting whether they come back `004d:d180` or `0xffff`.
I will post the result either way.

I also have a candidate DTS built from the above, never booted, with every
inference marked inline. Happy to post it here if it is useful rather than
noise, or to send it to anyone who wants to build it sooner than I can.

---

Disclosure, since it was asked earlier in the thread whether anyone had tried
pointing an LLM at this: yes, this was. The FIT extraction, the DTS reading and
the write-up were done with Claude. Every value quoted above is from the
decompiled vendor tree or from a named header, so it should all be checkable
against the image, and I would rather it were checked than taken on trust.
