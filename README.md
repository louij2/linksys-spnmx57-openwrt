# OpenWrt for the Linksys SPNMX57 (IPQ5018 / QCA8386 / QCA8084)

Unofficial OpenWrt support for the **Linksys SPNMX57**, the 2.5 GbE Velop-style
node several UK ISPs ship as a range extender.

The stock ISP firmware **will not do bridge or AP mode at all**, which is what
started this. On OpenWrt it does — plus per-port netdevs, real router mode, and
2.5 Gbps on the front ports.

> [!IMPORTANT]
> This is a personal port, not an official OpenWrt target. It has been proven on
> **one unit**. Read [docs/flashing.md](docs/flashing.md) before you flash
> anything, and have a UART cable to hand.

## What works

Verified on hardware, running from NAND on kernel 6.18.44:

| | status |
|---|---|
| `lan1` `lan2` `lan3` `wan` as separate netdevs | ✅ DSA, hardware bridged |
| 2.5 Gbps on the front ports | ✅ links and forwards |
| Router mode (NAT, DHCP, WAN) | ✅ end to end |
| Wi-Fi 6 — 2.4 GHz (IPQ5018) + 5 GHz (QCN9074) | ✅ both radios, clients associate |
| Dumb AP / bridge mode | ✅ the thing stock firmware refuses to do |
| `sysupgrade`, config persistence, reboot survival | ✅ |

Measured, iPhone over Wi-Fi 6 (HE80 2×2) with the device doing NAT on a 1 G
uplink: **485 Mbit/s down / 474 up**. See
[Performance](#performance) — two settings account for a 3× difference and are
now enabled by default.

## What does not work, or is untested

Be aware of these before you rely on it:

- **No NSS offload.** Wi-Fi and routed traffic cross the CPU (dual Cortex-A53).
  Wired-to-wired *is* hardware-switched and does not. NAT throughput will not
  reach 2.5 Gbps.
- **Wired-to-wired switching throughput is unmeasured.** It needs two 2.5 G
  hosts; the path is verified correct at the register level but has no number.
- **One unit, one person.** No second unit has ever run this build.
- Two cosmetic `rcg didn't update its configuration` warnings at boot (`mac0`,
  before the SerDes is up). Harmless; it settles on the correct rate.
- LEDs, buttons and per-port LED offload are not wired up.

## Install

Images are on the [Releases](https://github.com/louij2/linksys-spnmx57-openwrt/releases)
page. **Read [docs/flashing.md](docs/flashing.md) first** — it covers the dual
firmware partitions, the recovery path, and the UART pinout.

- Coming from **OpenWrt** → `...-squashfs-sysupgrade.bin` via `sysupgrade`
- Coming from **stock** → `...-squashfs-factory.bin`. This path is
  **not yet proven on this device**; do not attempt it without a UART attached.

Always verify against the `sha256sums` file on the release.

## Performance

Two settings, both now shipped enabled, that together nearly tripled download
throughput on the Wi-Fi path:

1. **Software flow offloading** (`/etc/config/firewall`). Hardware offload needs
   NSS and is unavailable. Note it is incompatible with SQM/QoS shaping.
2. **RPS packet steering** (`/etc/hotplug.d/net/20-packet-steering`). The conduit
   is single-queue, so without it one core handled every packet while the other
   idled — `IRQ 22` showed 1.36 M interrupts on cpu0 and **zero** on cpu1.
   The uci option `network.globals.packet_steering` does **nothing** on this
   build (netifd contains no `rps_cpus` code at all), hence the hotplug script.

| config | down | up |
|---|---|---|
| stock defaults | 164 | 385 |
| + flow offloading | 257 | 444 |
| + RPS | **485** | **474** |

## How it was done

The interesting part is the Ethernet. The QCA8386 switch had no DSA driver
anywhere — not in OpenWrt, not in mainline — so one was written, forked from
`qca8k`. Full write-up in [docs/CONTINUE-HERE.md](docs/CONTINUE-HERE.md).

The bug that cost the most: the driver inherited the **QCA8337's 32-bit MDIO
register decode**. The QCA8386 is addressed like the rest of the QCA8084
package — upper 16 bits at `reg | BIT(1)`, not `reg + 1`. Offset 0 decodes
identically under both schemes, so the chip ID read back perfectly while every
other register read 0 and every write was silently destroyed. Two plausible
theories (switch core held in reset; missing switch-core memory config) were
built and hardware-tested before the real cause was found by dumping a register
range and noticing every value was one 16-bit word duplicated into both halves.

## Credits

- **Hyndland** on the OpenWrt forum for the original `-22` analysis from the
  vendor GPL drop.
- **George Moussalem**, OpenWrt ipq50xx maintainer, who pointed at the uniphy
  soft-reset area — that named the exact problem.
- Qualcomm's `qca-ssdk` as the reference for the QCA8386 bring-up sequence.
- Mainline `qca8k` (John Crispin and others), which this driver is forked from.

## Discussion

- [OpenWrt Support for Linksys SPNMX57 variants](https://forum.openwrt.org/t/openwrt-support-for-linksys-spnmx57-variants/231653)
- [SPNMX57 / IPQ5018 / QCA8084 — `-22` root-caused (clock-parent liveness)](https://forum.openwrt.org/t/linksys-spnmx57-ipq5018-qca8084-22-root-caused-clock-parent-liveness-need-a-serial-confirm/252827)

## Licence

GPL-2.0, matching OpenWrt and the Linux kernel. See [LICENSE](LICENSE).
This repository contains work derived from OpenWrt, the Linux kernel, and
Qualcomm's `qca-ssdk`; those retain their own copyrights and licences.
