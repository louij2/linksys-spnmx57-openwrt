# linksys-spnmx57-openwrt

Getting Ethernet working on OpenWrt on the **Linksys SPNMX57** (Community
Fibre supplied, Qualcomm **IPQ5018**, vendor codename **Palm15**).

## Status

OpenWrt boots and everything except Ethernet works. There is a **usable release**
for anyone who wants the box as a wireless repeater in the meantime: see
[Releases](https://github.com/louij2/linksys-spnmx57-openwrt/releases).

| | |
|---|---|
| Kernel boot, UBI + overlay | works |
| Wi-Fi (QCN9074, ath11k) | works, all three interfaces at once |
| PCIe | works |
| LEDs, buttons, serial, SSH | works |
| sysupgrade + self-recovery | works |
| QCA8386 switch register access | works, reads and writes |
| **Ethernet** | **does not** |

The `-22` that this repo started from is long gone. The current position is
narrower and much better understood: the switch is fully reachable and the entire
vendor bring-up sequence has been reverse engineered and now runs correctly, with
all 27 of its register writes read back and verified on hardware. What has not
happened is the four QCA8084 EPHYs answering Clause 22 MDIO at addresses 1-4.

**The blocker that hid all of this was a chip held in reset.** Every register read
returned `0xffffffff` for three builds. `__mdiobus_register()` is what claims a
bus's reset GPIO and pulses it, so bring-up code that runs before registration
inherits that job -- and here the line (tlmm gpio24, `GPIO_ACTIVE_LOW`) was still
unclaimed with a pull-down, holding the QCA8386 in hardware reset for the whole
sequence. See [docs/HANDOVER.md](docs/HANDOVER.md) for the full current state, and
[docs/investigation.md](docs/investigation.md) for the original `-22` analysis.

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
