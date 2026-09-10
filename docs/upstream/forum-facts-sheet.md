# Raw facts for a forum post — write it yourself, in your own words

> **Status 2026-09-10: not posted, and not planned.** A generated post was flagged
> by the community and removed by a moderator. Luca decided not to rewrite it: the
> repo link is already in the existing threads, which was the whole point. This file
> is kept as reference only. **Do not draft a replacement post.**

The OpenWrt forum prohibits AI-generated technical content. This file is
reference data only: verified numbers and findings from your own work, to write
from. Do not paste it.

Post as a REPLY in the existing thread, not a new topic:
https://forum.openwrt.org/t/openwrt-support-for-linksys-spnmx57-variants/231653

## The headline

SPNMX57 works on current OpenWrt, kernel 6.18, mainline DSA stack (not qca-ssdk).
Repo: github.com/louij2/linksys-spnmx57-openwrt

Working: lan1/lan2/lan3/wan as separate DSA netdevs, 2.5G on the front ports,
router mode with NAT and DHCP, Wi-Fi 6 on both radios, dumb AP mode, sysupgrade,
survives reboots. Running from NAND.

Motivation worth mentioning: the stock ISP firmware won't do bridge/AP mode.

## The register decode bug (the bit other developers will care about)

There was no QCA8386 DSA driver anywhere - not OpenWrt, not mainline. Wrote one
forked from qca8k.

QCA8386 is NOT addressed like QCA8337. It uses the QCA8084 package scheme:

                    qca8k (wrong)        correct
  low register      (off >> 1) & 0x1e    off & 0x1f
  high register     r1 + 1               r1 | BIT(1)
  window / phy_id   (off >> 6) & 0x7     (off >> 5) & 0x7
  page              (off >> 9) & 0x3ff   (off >> 8) & 0xffff

Why it was hard: offset 0 decodes the same under both. So MASK_CTRL read back a
correct chip ID (0x17 rev 0x00) while every other register read 0x00000000 and
every write was destroyed - a 32-bit write emits low half then high half, and
r1+1 aliasing onto r1 means the high half overwrites the low.

Symptom looked identical to "switch core held in reset" or "gated clock". Both
were built and hardware-tested before finding the real cause.

What gave it away: dumped a register range, every value was one 16-bit word
duplicated into both halves - 0x17001700, 0x52525252, 0xc0a8c0a8.

Ground truth: __qca8084_mii_read() in drivers/net/phy/qcom/qca808x.c.

## Performance findings (possibly generic to ipq50xx)

Measured: iPhone over Wi-Fi 6 (HE80 2x2), device doing NAT, 1G uplink.

  OpenWrt stock defaults ................... 164 down / 385 up
  + software flow offloading ............... 257 / 444
  + threaded NAPI + packet_steering=1 ...... 342 / 424
  + RPS on the DSA user ports .............. 385 / 512

Finding A: packet_steering=1 alone did nothing here. OpenWrt implements it in
/usr/libexec/network/packet-steering.uc, a scheduler that distributes NAPI
*threads*. It does not create them and no uci option appears to. Threaded NAPI
defaults off, so on a fresh boot with the option set: eth0 rps_cpus=1, every DSA
port 0. IRQ 22 (eth0) showed 1,357,683 interrupts on cpu0 and 0 on cpu1.
Writing 1 to /sys/class/net/eth0/threaded then reloading packet_steering fixes
it - upstream then assigns napi/eth0-* to cpu0 and eth0 rps_cpus to cpu1 itself.
Worth asking the thread whether that is expected.

Finding B: upstream never sets RPS on DSA user ports - leaves lan*/wan at 0.
Setting them was worth 342->385 down, 424->512 up, and it survives a
packet_steering reload.

Note: IRQ staying on cpu0 is normal. RPS defers processing after the interrupt.

## Caveats to state honestly

- One unit. Only yours has run this.
- Stock -> OpenWrt untested. Yours arrived via sysupgrade from an existing
  OpenWrt install. factory.bin is built but nobody has taken that path.
- Wired-to-wired switching throughput unmeasured (needs two 2.5G hosts).
- No LED or button support.
- The "power cycle 4 times and it flips slots" auto-recovery does NOT apply:
  this build clears the boot counter properly, so auto-recovery only saves a
  device that never reaches userspace. UART is the real recovery path.

## Credits to include

- Hyndland: original -22 analysis from the vendor GPL drop
- George Moussalem: pointed at the uniphy soft-reset area on the old stack
- qca-ssdk as the reference for the QCA8386 bring-up sequence

## For LS3434 specifically

They have an SPNMX56 with NSS on 6.18 and asked about the SPNMX57. Worth telling
them: SPNMX56 is QCA8337, SPNMX57 is QCA8386. kuncy7's NSS work needs a
switch-specific module (qca8337-nss exists), so the 57 would need a qca8386-nss
written, and NSS is mutually exclusive with DSA user ports. They may be about to
buy hardware expecting a drop-in.
