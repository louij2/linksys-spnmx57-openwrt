# Investigation notes

> **Status: the `-22` is understood.** Read `hardware.md` first for what the
> board actually is. The sections below marked *superseded* are kept so the
> reasoning is traceable, but do not act on them.

## The error

```
Qualcomm QCA8084 90000.mdio-1:00: probe failed with error -22
```

`-22` is `-EINVAL`. The QCA8084 PHY driver matched, then rejected what it was
given.

## Root cause

Two separate things are wrong, and the second one is the nasty one. This
matches the analysis Hyndland posted to the forum on 19 Aug 2026, which was
derived from the vendor GPL `qca-ssdk` sources (`ssdk_mht_clk.c`,
`mht_sec_ctrl.c`, `ssdk_mht.c`), and which the vendor device tree we extracted
independently corroborates at every point.

### 1. The device tree does not describe a PHY package

The QCA8084 is not four independent PHYs. It is a **package** with shared
clocks, a shared reset and a shared clock controller. Mainline models this with
an `ethernet-phy-package` container node.

The current SPNMX57 DTS declares bare `ethernet-phy` stubs instead. So
`of_phy_package_join()` has no package to join them to and returns `-EINVAL`
per address. The driver never gets as far as touching the hardware.

What is missing:

- an `ethernet-phy-package@N` node with `compatible = "qcom,qca8084-package"`
- a `clock-controller@18` node for the package's own clock controller
  (`qca8084-nsscc`), plus `CONFIG_IPQ_NSSCC_QCA8K=y` in the kernel config
- the seven package clocks: `apb_bridge`, `ahb`, `sec_ctrl_ahb`, `tlmm`,
  `tlmm_ahb`, `cnoc_ahb`, `mdio_ahb`
- per PHY `clocks`/`resets` and `qcom,xpcs-channel`
- `pcs-phy` and `xpcs-phy` child nodes
- `qcom,phy-addr-fixup`, because the chip does not answer at 1..4 until it is
  strapped there

The vendor tree carries the same idea in its own vocabulary: `phyaddr_fixup`,
`uniphyaddr_fixup`, `mdio_clk_fixup` and a `fixup` flag on each PHY.

### 2. Clock parent liveness

`nss_cc_switch_core`, the APB bridge RCG, can only make 312.5 MHz from
`UNIPHY1_TX312P5M` divided by one. If the UNIPHY/PCS PLL feeding that parent is
not already running, `clk_set_rate(apb_bridge, 312500000)` itself returns
`-EINVAL`.

So even with a correct package node, the probe can still fail with the same
error code for a completely different reason. Two bugs, one symptom.

There is a related ordering bug: mainline straps the EPHY and PCS addresses
(the `0xc90f018` / `0xc90f014` read-modify-writes) *before* `package_clock_init`
deasserts the switch core and MDIO master clocks. `__qca8084_mii_read` only
checks `ret < 0`, so reads from an unclocked bus return `0xffff` and are
accepted as real values. The strap silently writes garbage.

### The hardware evidence agrees

yomod83706 scanned the flashed unit and got:

| bus | result |
|---|---|
| `mdio@88000` addr `0x07` | `0x004dd0c0` — the SoC's internal GE PHY, alive |
| `mdio@90000` all addresses | `0x00000000` — nothing answering at all |

A whole bus reading zero is a chip that is unclocked and unstrapped. It is not
a wrong address. That is bug 2 visible from userspace.

## The test that discriminates between the two bugs

Hyndland's request, and it is worth doing because it decides which fix is
needed. With the candidate DT and config booted, read `mdio@90000` addresses
1 to 4:

| read | meaning | fix needed |
|---|---|---|
| `004d:d180` | parent PLL was already live, probably from U-Boot | device tree work is enough |
| `0xffff` or `0x0000` | parent is dead | the driver reordering is also needed |

This is a single read on a serial connected unit. We have one.

## The load bearing unknown

**Mainline has no driver for the QCA8386 switch.**

Mainline's QCA8084 support models the **quad PHY**: four PHYs talking to the
SoC over 10G-QXGMII. This board is not wired that way. It has the QCA8386
**switch fabric** in front of those PHYs, and a single SGMII+ 2.5G link to the
SoC (`switch_mac_mode = MAC_MODE_SGMII_PLUS`, `forced-speed = 2500`).

`qca8k` covers the QCA8327 and QCA8337. It does not cover the QCA8386. The
vendor uses `qcom,ess-switch-qca8386` from its own SSDK, which is not in
OpenWrt.

That leaves three outcomes, and they are worth being honest about up front:

1. **The good case.** The QCA8386 comes out of reset defaulting to
   all-ports-forwarding. Fixing the `-22` clocks and straps the package, the
   PHYs link, and Linux sees **one** Ethernet interface with four sockets
   behind it behaving as a dumb switch. No per port control, no VLANs on the
   switch, but working Ethernet. For a home router that is a perfectly good
   result
2. **The middling case.** It does not forward by default, and it needs a small
   amount of register poking to be told to. Doable, but it is real work with no
   reference to copy
3. **The bad case.** It needs the SSDK, and this becomes a port rather than a
   device tree fix

Hyndland flagged exactly this and said explicitly that the analysis is "model
proven only, not silicon proven", and that they could not verify "that the
QCA8386 defaults to all-ports-forwarding in package-mode=switch".

**Nobody knows which case this is, and one boot on Luca's serial connected unit
answers it.** That is the highest value thing the hardware can do.

## A mismatch worth flagging before writing the DTS

`qcom,package-mode` in the mainline binding takes one of three values:

```
0  QCA808X_PCS1_10G_QXGMII_PCS0_UNUNSED
1  QCA808X_PCS1_SGMII_MAC_PCS0_SGMII_MAC
2  QCA808X_PCS1_SGMII_MAC_PCS0_SGMII_PHY
```

The vendor configuration is *PCS1 in SGMII+, PCS0 disabled*
(`switch_mac_mode = 0x0c`, `switch_mac_mode1 = 0xff`). **None of the three is
that.** Mode 1 is the closest, since PCS0 being unused makes its setting moot,
but this is an inference and not a fact. It is the single most likely thing in
the candidate DTS to be wrong.

---

## Superseded: the original guesses

Kept for traceability. The vendor tree has since answered all of these.

1. ~~Wrong PHY address, real device at `0x1c`~~ — no. Nothing is at `0x1c` on
   this board; `0x1c` was the **56**'s QCA8081. The real addresses are 1, 2, 3, 4
2. **Missing required properties** — correct, and much bigger than expected.
   The whole package container is missing
3. ~~The switch and 2.5G PHY may be on the wrong bus~~ — no. Both are on
   `mdio@90000`, and there is no separate 2.5G PHY
4. **GMAC mode and port mapping** — still open, see above

## Safety

A bad DTS means a device that does not boot. UART is what makes this cheap.
Booting a candidate from RAM in U-Boot rather than flashing is better still,
and is the first thing to establish once serial is attached.
