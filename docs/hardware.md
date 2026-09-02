# SPNMX57 hardware, as described by the vendor's own device tree

Everything on this page comes from `collected/vendor/vendor-palm15.dts`, which
was extracted from the stock OEM firmware image (md5 verified). It is the
vendor's description of this exact board, so it is the strongest evidence
available short of a multimeter. Line numbers below refer to that file.

Board codename: **Palm15**. Vendor tree self identifies as the Qualcomm
`IPQ5018/AP-MP03.1` reference board, adapted.

## Corrections to what we previously believed

| Previously assumed | Actually |
|---|---|
| A QCA8084 quad PHY, declared at address `00` | Four EPHYs at addresses **1, 2, 3, 4**, behind a **QCA8386 switch** |
| The 56's QCA8337 replaced by an 8084 | The 8386 replaces the 8337 **and** the 8081. There is no separate 2.5G WAN PHY |
| Two SoC MACs in use, as on the 56 | **One**. Only `dp1` exists, and it is MAC1 |
| PHY sits at `0x1c` somewhere | Nothing sits at `0x1c`. `0x1c` was the 56's QCA8081 |

The address `00` in the failing message is not the hardware. It is the DTS
being wrong, and the four real addresses are `1..4`.

## Topology

```
                IPQ5018
  +-----------------------------------+
  |  MAC0 --- internal GE PHY         |     mdio@88000, addr 7
  |           (declared, but no       |     confirmed live: ID 0x004dd0c0
  |            nss-dp node exists)    |
  |                                   |
  |  MAC1 --- UNIPHY1 ----------------+---- SGMII+ 2.5G forced ----+
  |           (dp1, qcom,id=2)        |                            |
  +-----------------------------------+                            |
                                                                   v
                                             +---------------------------------+
                                             |            QCA8386              |
                                             |  port0 = CPU, forced 2500/full  |
                                             |  port1 -- QCA8084 EPHY @1       |
                                             |  port2 -- QCA8084 EPHY @2       |
                                             |  port3 -- QCA8084 EPHY @3       |
                                             |  port4 -- QCA8084 EPHY @4       |
                                             +---------------------------------+
                                                  reached over mdio@90000
```

## MDIO buses

### `mdio@88000` (line 237)

```
compatible = "qcom,qca-mdio", "qcom,ipq40xx-mdio";
resets      gephy_mdc_rst
ethernet-phy@0 { reg = <0x07>; }        // IPQ5018 internal GE PHY
```

Independently confirmed on real hardware: yomod83706's scan on the flashed unit
read ID `0x004dd0c0` at address `0x07` on this bus.

### `mdio@90000` (line 251) — the one that fails

```
compatible          = "qcom,qca-mdio";
reg                 = <0x90000 0x64>;
pinctrl             mdc = gpio36, mdio = gpio37, drive-strength 8, bias-pull-up
phy-reset-gpio      = <&tlmm 24 0>;
phyaddr_fixup       = <0xc90f018>;
uniphyaddr_fixup    = <0xc90f014>;
mdio_clk_fixup;

ethernet-phy@0 { reg = <0x01>; fixup; }
ethernet-phy@1 { reg = <0x02>; fixup; }
ethernet-phy@2 { reg = <0x03>; fixup; }
ethernet-phy@3 { reg = <0x04>; fixup; }
```

Three things in there are the whole story:

- **`phyaddr_fixup` / `uniphyaddr_fixup`.** The QCA8084 does not answer at
  1..4 out of reset. The vendor driver writes those two registers to *strap*
  the EPHY and PCS MDIO addresses into place first. `0xc90f018` and
  `0xc90f014` are exactly the registers named in the root cause post
- **`mdio_clk_fixup`.** The package's internal clocks must be brought up before
  the strap write, or the write goes nowhere
- **`fixup` on each PHY.** Marks these as the addresses to be programmed, not
  addresses to be probed

This is why yomod83706's scan of `mdio@90000` returned `0x00000000` at every
address. The chip was not clocked and not strapped, so nothing was answering.
That is a dead bus, not a wrong address.

## The switches

Two `ess-instance` children, `num_devices = <0x02>`.

### `ess-switch@0x39c00000` (line 320) — the SoC side

```
compatible      = "qcom,ess-switch-ipq50xx";
switch_mac_mode = <0x0c>;               // MAC_MODE_SGMII_PLUS
cmnblk_clk      = "internal_96MHz";

port@0 { port_id = <1>; phy_address = <7>; }
port@1 { port_id = <2>; forced-speed = <2500>; forced-duplex = <1>; }
```

`0x0c` decodes via `dt-bindings/net/qcom-ipq-ess.h` in the qualcommax tree:

```
#define MAC_MODE_SGMII_PLUS   0xc
```

So the SoC to switch link is **SGMII+ at a forced 2.5G**, not 10G-QXGMII. That
matters, and it is checked below.

### `ess-switch1@1` (line 347) — the QCA8386

```
compatible          = "qcom,ess-switch-qca8386";
switch_access_mode  = "mdio";
mdio-bus            = <&mdio1>;          // the 0x90000 bus
switch_mac_mode     = <0x0c>;            // SGMII_PLUS, the uplink
switch_mac_mode1    = <0xff>;            // MAC_MODE_DISABLED, second uplink unused
switch_cpu_bmp      = <0x01>;            // port 0
switch_lan_bmp      = <0x1e>;            // ports 1,2,3,4
switch_wan_bmp      = <0x00>;            // none

port@0 { port_id = <0>; forced-speed = <2500>; forced-duplex = <1>; }
port@1 { port_id = <1>; phy_address = <1>; }
port@2 { port_id = <2>; phy_address = <2>; }
port@3 { port_id = <3>; phy_address = <3>; }
port@4 { port_id = <4>; phy_address = <4>; }
```

`switch_wan_bmp = 0` is worth noticing. There is no hardware WAN port. All four
front panel sockets are LAN on the switch, and WAN is a software distinction
the vendor firmware makes on top.

## The single MAC

There is exactly one `nss-dp` node in the whole tree (line 2034):

```
dp1 {
        compatible    = "qcom,nss-dp";
        qcom,id       = <0x02>;         // ESS port 2, i.e. MAC1
        reg           = <0x39d00000 0x10000>;
        interrupts    = <0 0x6d 4>;
        qcom,mactype  = <0x02>;         // Synopsys XGMAC, the 2.5G capable one
        phy-mode      = "sgmii";
};
```

No `dp` node exists for MAC0. The `port@0 { phy_address = <7>; }` entry in the
SoC side switch node is reference board boilerplate that this board does not
wire to a socket. The internal GE PHY answers on MDIO because it is inside the
SoC, not because there is a port on the back of the box.

Practically: **the SPNMX57 has one Ethernet interface as far as Linux is
concerned.** The four sockets are switch ports behind it.

## Other board facts from the same tree

| | |
|---|---|
| Buttons | wps on gpio27, reset on gpio28, both active low |
| LEDs | PWM red, green, blue |
| MDIO1 pins | mdc gpio36, mdio gpio37 |
| Package reset | gpio24 |
| Stock kernel | Linux 5.4.213, load/entry `0x41208000` |

## What is still not known from the vendor tree

- **Which socket is which port.** The vendor tree gives `port_id` but no
  labels, so the mapping from `port1..port4` to the physical LAN1..LAN3 and WAN
  markings on the case is unconfirmed. This needs plugging a cable in and
  watching which port links
- **Which ports are 2.5G capable.** The product page says 2x 2.5G and 2x 1G.
  The QCA8084 is a quad 2.5G PHY, so this is likely a magnetics or marketing
  limit rather than anything in the tree
- **Whether the QCA8386 forwards by default.** See `investigation.md`; this is
  the load bearing unknown for whether any of this produces working Ethernet
