# linksys-spnmx57-openwrt

Getting Ethernet working on OpenWrt on the **Linksys SPNMX57** (Community
Fibre supplied, Qualcomm **IPQ5018**, vendor codename **Palm15**).

## Status

OpenWrt boots and most of the device works. **Ethernet does not.**

| | |
|---|---|
| Kernel boot | works |
| PCIe | works |
| Wi-Fi (QCN9074, ath11k) | works |
| UBI + overlay | works |
| **Ethernet** | **fails** |

The failure:

```
Qualcomm QCA8084 90000.mdio-1:00: probe failed with error -22
```

`-22` is `-EINVAL`. **This is now root caused.** See
[docs/investigation.md](docs/investigation.md).

## What is known, and how

The vendor's own device tree has been extracted from the stock OEM firmware,
which is a public download and is itself a u-boot FIT image. No case was
opened and nothing was reflashed. It lives in
[collected/vendor/](collected/vendor/), with provenance and an md5.

That gives the real hardware, written up in
[docs/hardware.md](docs/hardware.md):

```
IPQ5018 MAC1 --UNIPHY1-- SGMII+ 2.5G forced --> QCA8386 switch
                                                  port0 = CPU
                                                  port1..4 = QCA8084 EPHYs
                                                             at MDIO addr 1,2,3,4
```

Not what was previously assumed. There is no PHY at address `0x1c` and none at
`00`; that was the SPNMX56's layout. There is exactly **one** SoC MAC in use,
and the four sockets are switch ports behind it.

## The two bugs behind the -22

1. **No PHY package in the device tree.** The QCA8084 is a package with shared
   clocks, a shared reset and its own clock controller. The current DTS
   declares bare `ethernet-phy` stubs, so `of_phy_package_join()` returns
   `-EINVAL` per address and the driver never touches the hardware
2. **Clock parent liveness.** The APB bridge RCG can only make 312.5 MHz from
   `UNIPHY1_TX312P5M`. If that PLL is not already running, `clk_set_rate()`
   itself returns `-EINVAL`. Same error code, different cause

Analysis credit: **Hyndland** on the OpenWrt forum, from the vendor GPL
`qca-ssdk` sources. The vendor device tree corroborates it at every point.

## The load bearing unknown

**Mainline has no driver for the QCA8386 switch.** `qca8k` covers the QCA8327
and QCA8337, not this. So the realistic best outcome is one Ethernet interface
with the four sockets behind it acting as a dumb switch, and that only holds if
the QCA8386 comes out of reset forwarding. Nobody has checked.

One boot on a serial connected unit settles it, and we have one.

## Next step

[docs/bench-test.md](docs/bench-test.md) — a single MDIO read over serial that
decides whether the device tree work is sufficient or whether the driver needs
reordering too. It needs no build.

Then [dts/ipq5018-spnmx57.dts](dts/ipq5018-spnmx57.dts), a candidate that has
never been booted, with every inference marked `[GUESS]` inline.

## Layout

| path | what |
|---|---|
| `collected/vendor/` | vendor DTB and DTS from the stock image, with provenance |
| `dts/ipq5018-spnmx56.dts` | the SPNMX56, for comparison. Do not treat as a base |
| `dts/ipq5018-spnmx57.dts` | candidate. **Never booted** |
| `docs/hardware.md` | what the board is, from the vendor tree |
| `docs/investigation.md` | the root cause, and what is still unknown |
| `docs/bench-test.md` | the one test to run next |
| `scripts/collect.sh` | read only ground truth dump from a running unit |

## Upstream

- Thread: [OpenWrt Support for Linksys SPNMX57 variants](https://forum.openwrt.org/t/openwrt-support-for-linksys-spnmx57-variants/231653)
- Root cause post: [-22 root-caused (clock-parent liveness)](https://forum.openwrt.org/t/linksys-spnmx57-ipq5018-qca8084-22-root-caused-clock-parent-liveness-need-a-serial-confirm/252827)

## Recovery

UART is what makes this practical: a bad DTS becomes a log to read rather than
a brick. Before the first flash, confirm whether U-Boot offers `tftpboot` plus
`bootm`, so candidates can be booted from RAM and nothing needs writing to
flash until something works. `docs/bench-test.md` covers checking that.
