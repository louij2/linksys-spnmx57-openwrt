# Handover prompt

Paste this into a fresh session to pick this project up.

---

Work on getting Ethernet working on OpenWrt on my **Linksys SPNMX57** (Community
Fibre supplied, Qualcomm IPQ5018). Read `docs/investigation.md`, `docs/hardware.md`,
`docs/build.md`, `docs/mdio-scan.md` and `docs/preinit-port.md` first, then this.

**Repo:** `~/Repositories/linksys-spnmx57-openwrt`, GitHub
`louij2/linksys-spnmx57-openwrt` — **now PUBLIC**. Forum thread:
https://forum.openwrt.org/t/openwrt-support-for-linksys-spnmx57-variants/231653
(a full progress update is drafted at `docs/upstream/forum-update.md` — check
whether it's been posted yet before drafting another).

**CURRENT STATE (2026-09-03):** OpenWrt boots on the SPNMX57, Wi-Fi works, LuCI
works, self-recovering flash cycle is proven reliable. Ethernet does not work
yet. **Serial console now works both directions** (TX and RX) — this took most
of a session to get right; see "Serial console" below before touching wiring.

**THE BIGGEST LEAD, chase this first:**

With a live U-Boot prompt (first time all session), the vendor's own U-Boot
printed this during its own network bring-up:

```
MAC0 addr:0:11:22:33:44:55
PHY ID1: 0x4d
PHY ID2: 0xd0c0
MAC1 addr:0:11:22:33:44:56
GMAC1:Get QCA8084_PHY
eth0
eth1
```

**`GMAC1:Get QCA8084_PHY`** — the vendor's U-Boot successfully identifies the
QCA8084 on MAC1, independent of Linux/qca-ssdk entirely. This is proof the chip
can be brought up correctly, and the U-Boot binary (`0:APPSBL`, mtd6) almost
certainly contains the exact strap/clock sequence we've been trying to
reverse-engineer for `mdio-ipq4019.c`. **Dump mtd6 and disassemble/string-search
it before writing any more driver code by hand** — this could shortcut the whole
remaining port. (We already disassembled part of mtd6 once this session, for
the bootcount ARM code — same technique applies: pull the partition, load as
raw ARM binary, find the GMAC1/QCA8084 init routine, read out the actual
register writes to `0xC90F018`/`0xC90F014`.)

**WHAT WE KNOW, in order of discovery:**

1. **The board.** QCA8386 switch, four QCA8084 EPHYs at MDIO 1-4 on `mdio@90000`,
   one SoC MAC (MAC1) on a forced 2.5G SGMII+ link, no hardware WAN port. Full
   detail + vendor DTB extraction recipe (no hardware needed) in
   `docs/hardware.md`
2. **Two DTS gotchas, both fixed, both required:** `&mdio0` must stay enabled
   even though MAC0 drives no socket (SSDK uses it as its default bus lookup);
   `&switch` must keep `port@0` for MAC0's internal GE PHY (ipq5018's
   `chip_ver_get` falls through to reading it). See `docs/build.md`
3. **Mainline QCA8084 PHY package route is a dead end on `qualcommax/ipq50xx`**
   — needs a PCS/uniphy clock provider that doesn't exist on this target (it
   does exist on `qualcommbe`/IPQ9574, and master has since moved qualcommax to
   stmmac/DSA + a `qca-uniphy` PCS driver on kernel 6.18 — revisit if this
   stalls, that may be the real long-term answer)
4. **SSDK route: `MHT_ENABLE=enable` works now.** Was compiled out
   (`MHT_ENABLE=disable` for every subtarget); enabling it needed a real patch —
   `qca808x.c`'s `match_phy_device` still uses the pre-6.12 one-arg signature,
   kernel 6.12 added a second param. Patch is in
   `package/kernel/qca-ssdk/patches/100-qca808x-match_phy_device-6.12-signature.patch`.
   **This is a genuine upstream qca-ssdk bug**, worth a standalone PR
5. **With MHT enabled, `ssdk_dt_parse[1446]:INFO:switch node is qca8386!`
   appears** — SSDK recognises the node. But `chip_ver_get` still fails:
   `qca_detect_phyid()` reads a plain MII ID at the DTS `phy_address` values
   (1-4), and the chip doesn't answer there yet
6. **Measured, not inferred, via a 32-address kernel MDIO scan already in the
   DTS:** no valid PHY ID anywhere on `mdio@90000`. `0x04/0x05` and `0x10-0x13`
   read `0x00000000`, `0x14-0x1f` read a repeating `0xb00eb00e`, everything else
   is `0xffff`. QCA8084 would be `0x004dd180`. See `docs/mdio-scan.md` for the
   full table and what it rules in/out
7. **Root cause, confirmed by code reading:** the EPHY addresses come from a
   strap register (`EPHY_CFG`, `0xC90F018`) that qca-ssdk **only ever reads**
   (`qca_mht_ephy_addr_get`, one call site, `grep` the whole tree to confirm)
   and never writes. The vendor does the strap + clock init in the **MDIO bus
   driver** at probe time (`qca_mht_preinit()` in QSDK's `mdio-qca.c`, not
   public). OpenWrt binds mainline `drivers/net/mdio/mdio-ipq4019.c`, which has
   none of it
8. **⚠ Safety finding: `MHT_ENABLE=enable` without also fixing the MDIO driver
   is a memory-safety bug, not just non-functional.** The SSDK expects
   `bus->priv` to be `struct qca_mdio_data` (with `sw_read`/`sw_write` function
   pointers ~64 bytes in); OpenWrt's `ipq4019_mdio_data` is a much smaller,
   different struct, allocated at its own smaller size via
   `devm_mdiobus_alloc_size`. Any code path that reaches `qca_mii_raw_read`
   reads past the allocation and calls garbage as a function pointer. Our image
   has not crashed only because `chip_ver_get` fails earlier via the normal
   `mdiobus_read()` API, which never reaches that path. **Full scope of what the
   real `mdio-ipq4019.c` port needs is in `docs/preinit-port.md`** — do not skip
   reading it, the struct layout mismatch is the part most likely to be gotten
   wrong

**SERIAL CONSOLE — now fully working, read this before touching wiring again:**

Both directions confirmed working as of this session's end. It took a long,
frustrating debugging arc to get here (garbled captures from a termios bug,
silent TX for most of the session, a physical header hunt across at least 3
pin sets). Key lessons, so nobody repeats them:

- **Hold the fd open BEFORE running `stty`.** `cat`/opening the device node
  alone silently resets macOS's termios speed to 9600, even after you `stty`
  it. Pattern that works: `exec 3<>$PORT` first, then `stty -f $PORT 115200 ...`
  while the fd is held, THEN start reading from fd 3. Every capture in
  `collected/` from before this was understood is suspect
- **The adapter enumerates as both `/dev/cu.usbserial-NNNN` and
  `/dev/cu.wchusbserial-NNNN`** (Apple's built-in driver and the WCH kext both
  claim it). Use the `wchusbserial` one; check `ls /dev/cu.*serial*` after every
  physical reconnect, the number changes
- **GND must not move once established.** A floating ground produces random
  low-value noise on every line (we saw stray bytes like `0x00`, `0xb00e...`
  patterns) that looks deceptively like "something is connected but wrong,"
  when it is actually "nothing is connected properly." If TX or RX goes silent
  or noisy after moving wires, check ground first
- **The single best test is: reboot + spam Enter for the full boot, one
  bash invocation, fd held open the whole time.** A clean multi-KB boot log
  proves RX+GND. Autoboot stopping into a `IPQ5018#` prompt proves TX. Testing
  against an already-booted OS (sending Enter and hoping for a shell prompt) is
  a much weaker signal — silence there is ambiguous, silence through a genuine
  reboot is not
- Device address moves on every reboot (DHCP lease). Find it by MAC
  `02:11:22:33:44:22` on the `10.0.0.0/24` LAN, not a fixed IP

**IMAGES:**

`bootcount` is deliberately left WITHOUT a `linksys,spnmx57` case in the
current test image — this makes it self-recovering (3 boot attempts, U-Boot
flips back to the known-good SPNMX56 slot on the 4th power-on). Do not add that
case back until Ethernet actually works end to end; it is the safety net.
`qca-nss-dp` is also currently excluded (`CONFIG_PACKAGE_kmod-qca-nss-dp` unset
in `.config`, NOT via `DEVICE_PACKAGES -pkg` which silently does not work on
this target) because it panics the kernel when it initialises against a switch
device the SSDK never registered. Put it back once `chip_ver_get` succeeds.

Build host: `srv-openstack` (tailnet), builds inside a `owrt-deb12` Docker
image because the host's own gcc 15/glibc 2.42 cannot compile OpenWrt's
bundled m4/gnulib. `~/spnmx57/openwrt` there is checked out at `05feabfd09`,
matching exactly what the flashed unit runs — do not update it casually,
feed/core version drift breaks LuCI (hit this once, see `docs/build.md`).

**RECOVERY, confirmed multiple times this session:** `auto_recovery=yes`,
`boot_part_ready=3`. Three boot attempts on the inactive slot, flips back on
the 4th power-on. `sysupgrade -F` is required (board name mismatch) and safe;
never `-n` (wipes the Wi-Fi config that is the only way back in), never `-s`
(overwrites the known-good slot), never the `factory.bin` via sysupgrade.

**NEXT STEPS, in order:**

1. Dump `0:APPSBL` (mtd6) and hunt for the `GMAC1:Get QCA8084_PHY` routine —
   likely the fastest path to the real register sequence
2. With that in hand (or failing that), write the `mdio-ipq4019.c` patch per
   `docs/preinit-port.md`: DT-gated `bus->priv` layout fix, `sw_read`/`sw_write`,
   `preinit` doing the EPHY_CFG/SERDES_CFG strap + clock init
3. Test via the 32-address MDIO scan already in the DTS — success is
   `0x004dd180` at addresses 1-4
4. Re-enable `qca-nss-dp`, confirm `chip_ver_get` succeeds and a netdev appears
5. Does the QCA8386 forward by default; socket-to-port mapping
6. Re-enable the `bootcount` case, remove the diagnostic scan, ship a real
   image; upstream everything (the qca808x.c fix alone is worth its own PR)

**HOW I LIKE TO WORK:** work the list without stopping to ask between items.
When something needs me, give the exact command, say what "worked" looks like,
then carry on and collect asks at the end. Verify on the actual hardware rather
than assuming. No dashes in prose you write for me. I now have a global rule in
`~/.claude/CLAUDE.md` asking you to proactively flag when a model or effort
switch would help — keep doing that.

**Ongoing outage note (2026-09-03):** Anthropic status page reported elevated
errors on Opus 5/4.8/4.6 and Fable 5.1 starting ~13:26 UTC, still under
investigation as of the last check. If Opus is unavailable, Sonnet 5 handled
this entire session's hardware/kernel work fine — don't treat model
availability as a blocker, just note which one you're on.

**Related, do not conflate:** `louij2/tp-link-m7350-signal-mod` (public) and
`louij2/m7350-openwrt` (private, stalled) are a different device.
