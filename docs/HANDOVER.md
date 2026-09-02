# Handover prompt

Paste this into a fresh session to pick this project up.

---

Work on getting Ethernet working on OpenWrt on my **Linksys SPNMX57** (Community
Fibre supplied, Qualcomm IPQ5018). Read `docs/investigation.md` and
`docs/hardware.md` first, then this.

**Repo:** `~/Repositories/linksys-spnmx57-openwrt`, GitHub
`louij2/linksys-spnmx57-openwrt` (**PRIVATE**, keep it that way for now).

**WHAT IT IS:** I flashed OpenWrt onto one of these. Kernel boots, PCIe, Wi-Fi
(QCN9074/ath11k) and UBI all work. **Ethernet does not.** The upstream thread is
https://forum.openwrt.org/t/openwrt-support-for-linksys-spnmx57-variants/231653
and the failure is:

```
Qualcomm QCA8084 90000.mdio-1:00: probe failed with error -22
```

`-22` is `-EINVAL`: the PHY driver matched, then rejected its device tree node.
A description problem, not a missing driver. IPQ5018 is already a supported
OpenWrt target, so no SoC bring-up is needed. This is DTS work.

**WHERE IT ACTUALLY STANDS (2026-09-02).** The `-22` is root caused and the
hardware is confirmed from the vendor's own device tree, which was extracted
from the public stock OEM image with no hardware access at all. Read
`docs/hardware.md`, `docs/investigation.md` and `docs/bench-test.md`.

Short version: one SoC MAC on a forced 2.5G SGMII+ link to a **QCA8386
switch**, whose four ports carry QCA8084 EPHYs at MDIO addresses **1, 2, 3, 4**.
Nothing at `0x1c` and nothing at `00`. Two bugs share the `-22`: a missing
`ethernet-phy-package` container, and a clock parent liveness violation.

The real blocker is that **mainline has no QCA8386 switch driver**, so the best
realistic outcome is one interface with the four sockets behind it as a dumb
switch, and only if the QCA8386 defaults to forwarding. Unverified.

`dts/ipq5018-spnmx57.dts` is a candidate that has never been booted.

**MY SETUP:**
- I have **more than one of these routers**, and only one is flashed. **The other
  is still on stock firmware** — its vendor DTB describes this exact board
  correctly, which beats inferring anything. Dumping it is probably the single
  highest-value first move.
- **UART headers are being fitted.** So a bad DTS is a boot log, not a brick, and
  we may be able to `bootm` a test kernel from RAM without flashing at all.
- Wi-Fi works on the flashed unit, so it is reachable over WLAN for `ssh` even
  with Ethernet dead. It is **not** on my tailnet.
- I am on a Mac. `dtc` may need installing (`brew install dtc`).

**START HERE, in this order:**
1. `docs/bench-test.md`. One MDIO read on the serial console decides whether
   the device tree work is sufficient or whether the driver needs reordering as
   well. It needs no build. Also confirm whether U-Boot has `tftpboot` and
   `bootm`, so candidates can be booted from RAM and never flashed.
2. Run `scripts/collect.sh` on the flashed unit over Wi-Fi and commit the
   output. Still worth having, mainly for `DISTRIB_TARGET` (which tree the
   image was built from) and the current `/sys/firmware/fdt`.
3. Backport the mainline QCA8084 series (patches 0301-0312 in `qualcommbe`) and
   set `CONFIG_IPQ_NSSCC_QCA8K=y`, then build `dts/ipq5018-spnmx57.dts`.
4. Boot it from RAM and read the log. Work the `[GUESS]` markers in the DTS in
   the order they are flagged; `qcom,package-mode` is the likeliest to be wrong.
5. Establish whether the QCA8386 forwards by default. This is the thing that
   decides whether any of it produces working Ethernet.
6. Post the result back to the thread either way, and reply to Hyndland's
   request for a serial confirm specifically. Someone there asked whether
   anyone had tried an LLM on this, so a clean write-up is worth something even
   if we fail.

**HOW I LIKE TO WORK:** work the list without stopping to ask between items. If
something needs me (a command on the router, a photo of the board, a decision),
give me the exact command in its own bash block, say what "worked" looks like,
then carry on with the next item and collect the asks at the end. Verify things
on the actual hardware rather than assuming — several bugs on my other project
came from confident guesses that were never checked. No dashes in prose you write
for me.

**Related:** `louij2/tp-link-m7350-signal-mod` (public) and `louij2/m7350-openwrt`
(private, stalled) are separate projects on a different device. Don't conflate
them.
