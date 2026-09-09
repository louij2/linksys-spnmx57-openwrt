# Forum post — release announcement

Target: [OpenWrt Support for Linksys SPNMX57 variants](https://forum.openwrt.org/t/openwrt-support-for-linksys-spnmx57-variants/231653)
Cross-link from: [SPNMX57 / IPQ5018 / QCA8084 — `-22` root-caused](https://forum.openwrt.org/t/linksys-spnmx57-ipq5018-qca8084-22-root-caused-clock-parent-liveness-need-a-serial-confirm/252827)

---

**SPNMX57: working build on the mainline DSA stack — router mode, 2.5G, Wi-Fi 6**

Following on from the `-22` clock-parent work in the other thread, I have the
SPNMX57 running on current OpenWrt (kernel 6.18) with the **mainline DSA
ethernet stack**, not qca-ssdk. Images and full write-up here:

https://github.com/louij2/linksys-spnmx57-openwrt

The thing that started this: the stock ISP firmware **will not do bridge or AP
mode at all**. These get handed out as range extenders and you cannot make them
behave as dumb APs. On OpenWrt they will.

**Working, verified on hardware, running from NAND:**

* `lan1` `lan2` `lan3` `wan` as separate DSA netdevs, hardware bridged
* 2.5 Gbps on the front ports
* Router mode end to end — NAT, DHCP, WAN
* Wi-Fi 6 on both radios (IPQ5018 2.4 GHz + QCN9074 5 GHz); clients associate at
  the full 2×2 HE80 rate, MCS 11, zero retries
* Dumb AP / bridge mode
* `sysupgrade` works and the firmware survives reboots

**The interesting part — there was no QCA8386 DSA driver anywhere.** Not in
OpenWrt, not in mainline. Qualcomm's claim that it "works with qca8k.c with
supplement patches" has no posted patch behind it. So I wrote one, forked from
`qca8k`.

The bug that cost the most time is worth sharing, because it is a trap anyone
forking `qca8k` for this family will hit. **The QCA8386 does not use the
QCA8337's 32-bit MDIO register decode.** It is addressed like the rest of the
QCA8084 package:

```
                  qca8k (wrong here)   QCA8084 package (correct)
  low register    (off >> 1) & 0x1e    off & 0x1f
  high register   r1 + 1               r1 | BIT(1)
  window/phy_id   (off >> 6) & 0x7     (off >> 5) & 0x7
  page            (off >> 9) & 0x3ff   (off >> 8) & 0xffff
```

**Offset 0 decodes identically under both schemes.** So `MASK_CTRL` returned a
perfectly correct chip ID and revision the whole time, while every other
register read `0x00000000` and every write was silently destroyed — a 32-bit
write emits the low half then the high half, and with `r1 + 1` aliasing onto
`r1` the high half overwrites the low one. Setting a low bit like
`GLOBAL_FW_CTRL0.CPU_PORT_EN` could never read back as anything but zero.

That looks *exactly* like a switch core held in reset, or a gated clock. I built
and hardware-tested both of those theories before finding the real cause. What
gave it away was dumping a register range and noticing every value was one
16-bit word duplicated into both halves — `0x17001700`, `0x52525252`,
`0xc0a8c0a8`. That is the signature of reading the same MDIO register twice.
Ground truth was `__qca8084_mii_read()` in `drivers/net/phy/qcom/qca808x.c`,
sitting in the same tree the whole time.

**If you are debugging this chip: dump a range and look at the shape of the
values before theorising about clocks or resets.**

**Performance, and two settings worth knowing about.** There is no NSS offload
on this stack, so anything crossing the CPU is limited by the dual A53.
Wired-to-wired *is* hardware-switched and unaffected. Measured from an iPhone
over Wi-Fi 6 with the device doing NAT on a 1 G uplink:

| config | down | up |
|---|---|---|
| OpenWrt stock defaults | 164 | 385 |
| + software flow offloading | 257 | 444 |
| + threaded NAPI, upstream packet steering | 342 | 424 |
| **+ RPS on the DSA user ports** | **385** | **512** |

**+135% down, +33% up.** All of it ships enabled in the images. Three findings,
and I think the last two are generic to ipq50xx rather than specific to my unit,
so they may be worth a look on other boards:

1. Software flow offloading was off. Hardware offload needs NSS, so on this stack
   software offload is the whole budget. Note it is incompatible with SQM shaping.

2. **`packet_steering=1` on its own did nothing for me.** This one surprised me.
   OpenWrt implements it in `/usr/libexec/network/packet-steering.uc`, which is a
   scheduler that distributes NAPI *threads* across CPUs — but it does not create
   them, and I could not find a uci option that does. Threaded NAPI is off by
   default here, so with the option set I still measured, on a fresh boot,
   `eth0 rps_cpus=1` and every DSA port at `0`: the whole datapath on cpu0 with
   cpu1 idle. `IRQ 22 (eth0)` showed 1,357,683 interrupts on cpu0 and zero on
   cpu1. Writing `1` to `/sys/class/net/eth0/threaded` and reloading
   `packet_steering` fixes it — upstream then assigns `napi/eth0-*` to cpu0 and
   `eth0 rps_cpus` to cpu1 by itself, which is exactly right.

   Is that expected? It looks to me like the option is a no-op on any target
   where threaded NAPI is not already enabled, which would make it quietly
   ineffective on a lot of hardware. Happy to be told I have misread it.

3. **Upstream never sets RPS on the DSA user ports.** It manages the conduit and
   the wifi netdevs but leaves `lan*`/`wan` at `rps_cpus=0`. Setting them was
   worth 342→385 down and 424→512 up, and it survives a `packet_steering` reload,
   so it composes rather than conflicts.

For the avoidance of doubt: `IRQ 22` staying entirely on cpu0 is normal. RPS
defers the processing *after* the interrupt, it does not move the interrupt.

**Caveats, stated plainly:**

* **One unit.** Only mine has ever run this.
* **Stock → OpenWrt is untested.** Mine was already on OpenWrt, so it arrived by
  `sysupgrade`. `factory.bin` is built and is the right image, but nobody has
  taken that path. If you try it, have a UART on it, and please report back.
* Wired-to-wired switching throughput is unmeasured — needs two 2.5 G hosts.
  The path is verified correct at register level (the bridged ports carry each
  other in the switch's own port-member mask, and `wan` correctly does not) but
  I have no number for it.
* No LED or button support yet.
* The auto-recovery "power cycle four times and it flips slots" behaviour does
  **not** apply to this build. It clears the boot counter properly (which is
  correct, and stops spurious reverts), so auto-recovery only saves you from a
  device that never reaches userspace. UART is the real recovery path.

Credit where it is due: **Hyndland** for the original `-22` analysis from the
vendor GPL drop, and **George Moussalem** for pointing at the uniphy soft-reset
area — that named the exact problem on the old stack. `qca-ssdk` was the
reference for the QCA8386 bring-up sequence throughout.

Happy to answer questions, and very interested in anyone with a second unit or
an SPNMX56 who wants to test.
