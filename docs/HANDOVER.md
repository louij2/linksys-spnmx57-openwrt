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

**THE KEY FINDING SO FAR** (in `docs/investigation.md`): this is *not* the
SPNMX56 with a tweak. The 56 has a QCA8337 five-port switch **plus** a separate
QCA8081 2.5G PHY at 0x1c. The 57 fails on a **QCA8084** — a quad PHY that
integrates both — declared at address **00**. The topology differs, so the DTS
needs re-describing rather than patching. `dts/ipq5018-spnmx56.dts` is in the
repo as the base to work from.

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
1. Get ground truth. Run `scripts/collect.sh` on the flashed unit over Wi-Fi and
   commit the output to `collected/`. The valuable parts are `/sys/firmware/fdt`
   (the exact tree the kernel booted with) and `/sys/bus/mdio_bus/devices/`
   (which PHYs really enumerated, at which addresses).
2. Establish what the hardware actually is: chip markings on the board while it
   is open for UART, plus the MDIO scan. Confirm QCA8084 versus 8337+8081 before
   writing anything.
3. Look for a Linksys **GPL source dump for the 57**. One exists for the 56
   (`domenpk/Linksys_SPNMX56TB_v1.0.1.216589`). If a 57 equivalent exists, its
   device tree answers everything.
4. Only then write the DTS: switch node, PHY address, MDIO bus structure, GMAC
   modes, port mapping.
5. Test with UART attached, from RAM if U-Boot allows it.
6. Post the result back to the thread either way. Someone there asked whether
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
