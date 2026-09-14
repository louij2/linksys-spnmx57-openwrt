# OpenWrt for the Linksys SPNMX57 (IPQ5018 / QCA8386 / QCA8084)

Unofficial OpenWrt support for the **Linksys SPNMX57**, the 2.5 GbE Velop-style
node several UK ISPs ship as a range extender.

The stock ISP firmware **will not do bridge or AP mode at all**, which is what
started this. On OpenWrt it does — plus per-port netdevs, real router mode, and
2.5 Gbps on the front ports.

> [!IMPORTANT]
> This is a personal port, not an official OpenWrt target. It has been proven
> on **two units**, both flashed straight from stock, no case opening, no UART
> needed — see below. Read [docs/flashing.md](docs/flashing.md) for the full
> picture (recovery, dual partitions, caveats) before you flash anything.

## Quick start — flashing from stock

1. **Download**, and check the hash:
   - [`...-squashfs-factory.bin`](https://github.com/louij2/linksys-spnmx57-openwrt/releases/latest/download/openwrt-qualcommax-ipq50xx-linksys_spnmx57-squashfs-factory.bin) — this is the one you want, coming from stock
   - [`sha256sums`](https://github.com/louij2/linksys-spnmx57-openwrt/releases/latest/download/sha256sums) — verify the download against this before flashing anything
2. **Power cycle the router** (pull the mains lead, not a reboot) so it's freshly booted.
3. Cable it **LAN port → your computer**, nothing else attached.
4. Browse to **`https://<router-ip>/fwupdate.html`** — a hidden page Linksys
   document themselves, not reachable from the normal admin UI. Accept the
   self-signed certificate warning.
5. Log in (factory-fresh default is `admin`; otherwise whatever you set in
   the setup wizard), choose the `factory.bin` you downloaded, click **Update**.
6. Leave it alone — it writes and reboots itself. About a minute later it
   comes up as OpenWrt on `192.168.1.1`, SSH open, no root password set.

No UART, no case opening, no signed firmware needed. Full detail, what to
expect at each step, and the recovery path if anything goes sideways:
**[docs/flashing.md](docs/flashing.md#from-stock--proven-no-uart-no-case-opening)**.

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
uplink: **385 Mbit/s down / 512 up** — up from 164/385 on stock defaults. See
[Performance](#performance) — two settings account for a 3× difference and are
now enabled by default.

## What does not work, or is untested

Be aware of these before you rely on it:

- **No NSS offload.** Wi-Fi and routed traffic cross the CPU (dual Cortex-A53).
  Wired-to-wired *is* hardware-switched and does not. NAT throughput will not
  reach 2.5 Gbps.
- **Wired-to-wired switching throughput is unmeasured.** It needs two 2.5 G
  hosts; the path is verified correct at the register level but has no number.
- **`lan1` and `lan2` have never had anything plugged into them.** They
  link-detect and are configured, but are untested with a real partner.
- Two cosmetic `rcg didn't update its configuration` warnings at boot (`mac0`,
  before the SerDes is up). Harmless; it settles on the correct rate.
- LEDs, buttons and per-port LED offload are not wired up.

## Install

Coming from **stock**? See [Quick start](#quick-start--flashing-from-stock)
above.

Already on **OpenWrt**? Grab
[`...-squashfs-sysupgrade.bin`](https://github.com/louij2/linksys-spnmx57-openwrt/releases/latest/download/openwrt-qualcommax-ipq50xx-linksys_spnmx57-squashfs-sysupgrade.bin)
and `sysupgrade` it. All images are on the
[Releases](https://github.com/louij2/linksys-spnmx57-openwrt/releases) page —
**read [docs/flashing.md](docs/flashing.md)** for the dual firmware
partitions, the recovery path, and the UART pinout.

Both the stock→OpenWrt path and a UART-based recovery flash have now been
exercised on a second, separately-purchased unit.

Always verify against the `sha256sums` file on the release.

## Performance

Two settings, both now shipped enabled, that together nearly tripled download
throughput on the Wi-Fi path:

1. **Software flow offloading** (`/etc/config/firewall`). Hardware offload needs
   NSS and is unavailable. Note it is incompatible with SQM/QoS shaping.
2. **Packet steering**, via `network.globals.packet_steering` plus
   `/etc/init.d/spnmx57-netperf`. The conduit is single-queue, so without it one
   core handles every packet while the other idles — `IRQ 22` showed 1.36 M
   interrupts on cpu0 and **zero** on cpu1.

   Two things are needed beyond setting the uci option, and both are shipped:
   **threaded NAPI** on the conduit (OpenWrt's `packet-steering.uc` distributes
   NAPI *threads*, but nothing creates them, so the option alone is a no-op
   here), and **RPS on the DSA user ports**, which upstream never touches. All
   CPU placement decisions are left to upstream.

| config | down | up |
|---|---|---|
| OpenWrt stock defaults | 164 | 385 |
| + software flow offloading | 257 | 444 |
| + threaded NAPI, upstream steering | 342 | 424 |
| **+ RPS on the DSA user ports (shipped)** | **385** | **512** |

## How it was done

The interesting part is the Ethernet. The QCA8386 switch had no DSA driver
anywhere — not in OpenWrt, not in mainline — so one was written, forked from
`qca8k`. Full write-up in [docs/newstack-porting-log.md](docs/newstack-porting-log.md),
and the complete debugging story — including the earlier vendor-SSDK port that
came before it — is indexed in **[docs/README.md](docs/README.md)**.

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
