# CONTINUE HERE — SPNMX57 new-stack port (live state, 2026-09-09)

Branch `newstack-port`. **Memory does not sync between machines — this file plus
`docs/` is the context.** Also read: NEWSTACK-PORT-PLAN.md,
NEWSTACK-QCA8386-DSA-DESIGN.md, NEWSTACK-PHASE2B-KIT.md,
phase2b-research/UNRESOLVED-CHECKLIST.md, NEWSTACK-BENCH-PLAN.md.

## Status: the 2.5G traffic path WORKS on hardware

Confirmed live over the serial console on the new DSA stack (kernel 6.18.44):

```
qca8386 90000.mdio-1:10: detected QCA8386 (id 0x17 rev 0x00)
qca8386 90000.mdio-1:10: Link is Up - 2.5Gbps/Full          <- switch side
DSA: tree 0 setup
eth0: configuring for fixed/2500base-x link mode
eth0: Link is Up - 2.5Gbps/Full - flow control rx/tx        <- SoC conduit
br-lan: port 1(eth0) entered forwarding state
```

- ✅ Phase 1 boot, Phase 2a (QCA8084 + nsscc deps), Phase 2b (`qca8386` DSA driver)
- ✅ **Phase 2c — CPU-port conduit at 2.5G** (see "the conduit fix")
- ✅ **Wi-Fi** — both radios register (see "the wifi fix")
- ✅ **Phase 2d — user ports LINK.** nsscc probes, the QCA8084 package binds all
  four EPHYs, and `lan1-4` DSA netdevs come up at the right speeds:
  `lan3: Link is Up - 2.5Gbps/Full`, `lan4: Link is Up - 1Gbps/Full`.
  (They are administratively down at boot until brought up / configured.)
- ✅ **FORWARDING WORKS** (2026-09-09). Root cause was the 32-bit MDIO register
  decode, not clocks and not reset - see "the forwarding blocker".
- ✅ **ROUTER MODE WORKS END TO END.** `wan` takes a DHCP lease from the upstream
  LAN, the default route goes via `wan`, and the box pings `1.1.1.1` in 3.6 ms.
  `lan1-3` bridge in `br-lan` on 192.168.1.1, `lan3` links and forwards at 2.5G
  (0.8 ms RTT, ssh from a cabled host works), both wifi phys register.
- ✅ **FLASHED TO NAND AND PERSISTS.** `sysupgrade -n` writes partition 1
  (`mtd12`/`mtd13`), U-Boot flips `boot_part` to 1, and a plain `reboot` comes
  back up on `kernel-1` / `ubi0: attached mtd13` running 6.18.44 - it no longer
  falls back to v0.4.0. **v0.4.0 is untouched on partition 2 as the fallback.**
- ⏳ then: release notes, wider testing

## The wifi fix (committed)

`linksys,spnmx57` was missing from the ipq50xx `11-ath11k-caldata` hotplug
script - the only thing that carves per-device cal out of the `0:art` partition
into `/lib/firmware`. Without it ath11k fails with `failed to load board data
file: -12` and registers no phys. Adding spnmx57 to spnmx56's case arms
(`0:art @ 0x1000` built-in, `@ 0x26800` QCN9074) fixes it: `/sys/class/ieee80211`
now shows phy0+phy1 and both `cal-*.bin` are extracted. This also proves
spnmx57 shares spnmx56's ART layout.

## The forwarding blocker — SOLVED (2026-09-09)

**It was the 32-bit MDIO register decode.** `qca8386.c` was forked from `qca8k`
and inherited the QCA8337's addressing. The QCA8386 is addressed like the rest
of the QCA8084 package:

```
                  qca8k (wrong here)   QCA8084 package (correct)
  low register    (off >> 1) & 0x1e    off & 0x1f
  high register   r1 + 1               r1 | BIT(1)
  window/phy_id   (off >> 6) & 0x7     (off >> 5) & 0x7
  page            (off >> 9) & 0x3ff   (off >> 8) & 0xffff
```

Ground truth: `__qca8084_mii_read()` / `__qca8084_set_page()` in
`drivers/net/phy/qcom/qca808x.c`, which drive this same chip.

**Offset 0 decodes identically under both schemes**, so `MASK_CTRL` returned a
correct chip id (0x17 rev 0x00) the whole time while every other register read 0
and every write was dropped - a 32-bit write emits the low half then the high
half, and with `r1 + 1` aliasing onto `r1` the high half overwrites the low one.
`FW_CTRL0` went from `0x00000000` to `0x001004f0` the moment this was fixed.

**How it was found:** sweeping the switch core through the debugfs knob returned
177 non-zero registers with plausible defaults - so the core was never dead -
and *every value* was one 16-bit word duplicated into both halves
(`0x17001700`, `0x52525252`, `0xc0a8c0a8`), the signature of reading the same
MDIO register twice. **If you are ever back here: dump a range and look at the
shape of the values before theorising about clocks or resets.**

### The wan (port 4) follow-on, also solved

With forwarding up, `lan3` passed traffic but `wan` sat at rx=0. `mac4_tx/rx`
logged `rcg didn't update its configuration`: `__clk_rcg2_select_conf()` takes
the first config whose parent gives an exact rate match, and
`ftbl_nss_cc_mac4_tx_clk_src_125` lists `C(P_UNIPHY0_RX, 1)` *before*
`C(P_UNIPHY1_TX312P5M, 2.5)`. Our DT advertised a 125MHz `uniphy0_rx` stub, so
the RCG tried to source port 4 from UNIPHY0 - which does not run on this board
(switch mode brings up UNIPHY1 only; the vendor even asserts SRDS0). Declaring
the two UNIPHY0 stubs at **0 Hz** makes them unmatchable and the selector falls
through to the parent the vendor mandates. wan rx 0 -> 134.

### Kept but NOT proven necessary

The switch-core reset (`NSS_CC_SWITCH_CORE_ARES` + MAC0..5) and the switch-core
memory config (patch 0944, MEM_CTRL 0xc90f044 / MEM_ACC 0xc90f048) are both
still in. The vendor does both. But both were tested against a driver that could
not write a register, so neither has been shown to be required. If you want to
slim the port down, these are the first two things to try removing - one at a
time, with the `FW_CTRL0=` dmesg line as the check.

## (historical) What the symptoms looked like before the decode fix

**The whole switch core reads back zero.** The debugfs knob
(`/sys/kernel/debug/90000.mdio-1:10/reg`, write a hex offset then read) showed:

```
MASK_CTRL id/rev   0x0    -> 0x17001700   <- correct
PORT_STATUS0 cpu   0x7c   -> 0x00000000
PORT_HDR_CTRL0     0x9c   -> 0x00000000
GLOBAL_FW_CTRL0    0x620  -> 0x00000000
GLOBAL_FW_CTRL1    0x624  -> 0x00000000
PORT_LOOKUP0 cpu   0x660  -> 0x00000000
```

The MDIO window works (the ID register answers, and 0x0 and 0x7c are on the
*same page*, so this is not a paging bug). Only the hard-wired ID register
answers; every SRAM-backed switch register reads 0 and writes are dropped.

Root cause: **in switch mode the QCA8386's switch-core memories must be handed
over explicitly**, and the vendor requires it *before* UNIPHY1 is brought up:

```
qca_mht_mem_ctrl_set(dev_id, MHT_MEM_CTRL_DVS_SWITCH_MODE, MHT_MEM_ACC_0_SWITCH_MODE)
  MEM_CTRL 0xc90f044: clear GENMASK(9,4), set BIT(5)   (DVS_RAWA_ASSERT)
  MEM_ACC  0xc90f048: 0x000c0c0c
```
> vendor comment: *"for switch mode, the uniphy1 must be initialized firstly and
> initialized only one time, so configure dvs and acc for memory before uniphy1
> initialization"* — `mht_interface_ctrl.c`

Mainline's `qca808x.c` writes `WORK_MODE_CFG` (0xc90f030) but **never touches
MEM_CTRL/MEM_ACC**, because upstream only supports the PHY/QXGMII modes.
Delivered as **patch 0944**.

### Ruled out along the way (do not re-litigate)

- **DSA tagging** — `eth0` rx is *exactly* 0, so nothing reaches the MAC at all.
- **Clock gating alone** — `clk_ignore_unused` on the cmdline changed nothing.
- **The RCG warnings** (`nss_cc_switch_core_clk_src: rcg didn't update its
  configuration`) — real, and fixed by the SerDes bring-up in patch 0943; the
  warning count is now 0 and `switch_core`/`srds1_sys` both read enable=1.
  Forwarding still did not work, so this was necessary but not sufficient.
- **Switch core held in reset** — plausible and tested: claiming
  `NSS_CC_SWITCH_CORE_ARES` + MAC0..5 TX/RX from nsscc and pulsing them at the
  top of `qca8386_setup()` runs clean, and `FW_CTRL0` *still* read 0x00000000.
  Kept anyway (the vendor does it), but it is **not** the fix.

`qca8386_setup()` now logs `FW_CTRL0=` after programming, so a single dmesg line
tells you whether the core is alive — non-zero means the memory config landed.

Still unverified alongside it: `pcs-phy@6` maps `pcs_rx_root` to the TX
312.5MHz clock and `pcs_tx_root` to the RX one - verify against the binding
before changing (do not "fix" it blind).

## The conduit fix (committed)

The DSA conduit `eth0`/gmac1 used to fail `validation of 2500base-x ... -EINVAL`
(supported mask `0x6280` = port/pause bits, **no speed bits**). Two causes:

1. `dwmac-ipq5018.c` only added `MAC_2500FD` + `PHY_INTERFACE_MODE_2500BASEX`
   inside `if (priv->hw->phylink_pcs)` — a **runtime** pointer that is NULL when
   caps are built (`pcs_init` silently returns 0 on a missing PCS). Fixed by
   gating on the **DT property** instead:
   `if (fwnode_property_present(dev_fwnode(priv->device), "pcs-handle"))`
   (+ `#include <linux/property.h>`).
2. The phy-less conduit had no `fixed-link`, so phylink ran `MLO_AN_PHY` and
   tried to attach a PHY that doesn't exist. Fixed by adding to `&gmac1`:
   `fixed-link { speed = <2500>; full-duplex; pause; };`
   **dtc requires properties before subnodes** — put `fixed-link` last in the node.

Files: `newstack/target/linux/qualcommax/files/drivers/net/ethernet/stmicro/stmmac/dwmac-ipq5018.c`,
`newstack/target/linux/qualcommax/dts/ipq5018-spnmx57.dts`.

## The iteration loop (non-destructive — use this)

`newstack/tools/ramboot-test.py` boots an initramfs **from RAM via U-Boot TFTP**.
Nothing is written to NAND; a power-cycle always recovers. Never sysupgrade to
iterate — an image that doesn't boot leaves the box stuck (see below).

Setup: USB-serial to the device UART; an ethernet cable from a host NIC to a
device **front port**; that NIC given a static IP; a TFTP server on the host.
Then drop the built `-initramfs-uImage.itb` into the TFTP root and run the tool.

Known flakiness the tool handles: the U-Boot autoboot window is short (retries),
and the port needs a few seconds to link after a reboot before TFTP works
(pings until the peer answers).

**Trap that cost a cycle:** the harness fails with `peer never answered` if the
*host* loses its route to the direct-cable subnet. On the Mac, `en18` held a
static 192.168.1.10 **and** later picked up a DHCP lease from the upstream LAN
(the switch bridges all ports whenever no OS is driving it, e.g. while sitting
in U-Boot). macOS then reconfigured the interface and dropped the
192.168.1.0/24 route, so `route -n get 192.168.1.1` resolved via the *default
gateway* and TFTP replies never went down the cable - while `ping -b en18` still
worked and made it look fine. Check `route -n get <box ip>` before blaming the
board. Restore with:

    sudo ifconfig en18 192.168.1.10 netmask 255.255.255.0 up

A working fallback when that happens: the box's `wan` port is on the upstream
LAN, so you can TFTP over that instead -
`setenv ipaddr <free LAN ip>; setenv serverip <host LAN ip>; tftp 0x44000000 spnmx57.itb`
(use `setenv` only, never `saveenv`).

## U-Boot facts (vendor U-Boot, prompt `IPQ5018#`)

- Commands are `tftp` (NOT `tftpboot`), `bootm`, `nand read/write`, `printenv`.
- **Dual firmware**: `boot_part=1` → `bootpart1` (mtd12 `kernel` + mtd13 `rootfs`);
  `boot_part=2` → `bootpart2` (mtd14 `alt_kernel` + mtd15 `alt_rootfs`).
  So you can toggle between the new stack and v0.4.0 with
  `setenv boot_part N; saveenv; reset` — no flashing.
- `auto_recovery=yes` + a boot_count that U-Boot increments each boot: if
  userspace never resets it, U-Boot **flips `boot_part`**. The new-stack image
  does not reset boot_count, which is what looked like a "revert" earlier.
- Front ports are visible in U-Boot (`PORT3 Up Speed :2500 Full duplex`), and
  U-Boot uses `eth1` for the switch path.

## Hard-won lessons

- **Never remotely flash when the only recovery path routes through the target.**
  Flashing the box killed the serial bridge (it was networked *through* the box)
  and left it unrecoverable remotely. Use RAM boot + a UART that is independent.
- An image that boots to a shell but has no working network looks identical to
  "bricked" from outside. Check the serial before concluding anything.

## Environment

- Build host: `ssh srv-openstack`, tree `/tmp/owrt-main` (**tmpfs — volatile**),
  container `owrt-build`. Persistent artifacts: `~/spnmx57-newstack-artifacts/`.
- **Run `make defconfig` INSIDE the container** — the host's python is 3.14 and
  bakes a dangling `staging_dir/host/bin/python3` symlink; the container has 3.11.
- Old-stack v0.4.0 (the shipped, working release) is a separate tree and is the
  fallback firmware on the other partition.

## Measured on hardware (2026-09-09, new stack)

| what | result |
|---|---|
| `lan3` link | 2.5 Gbps full, forwards, 0.8 ms RTT, ssh works |
| `wan` link | 1 Gbps full, DHCP lease from upstream, default route |
| internet | `ping 1.1.1.1` 3.6 ms from the box |
| wifi | `phy0` + `phy1` register, both cal blobs extracted |
| conduit `eth0` | 2.5 Gbps full, flow control rx/tx |
| CPU-terminated throughput | ~293 Mbit/s single-stream HTTP to `/dev/null`, and
  `sys` time was 9.4 s of 11.5 s - the A53 is the limit, there is no NSS offload
  on this stack. Port-to-port switching is done in the switch hardware and does
  not touch the CPU, so this number is **not** the front-panel switching rate.
  A proper NAT/forwarding benchmark still needs a second cabled host. |
| `rcg didn't update` warnings | 2, both `mac0` during early boot before the
  SerDes is up; `mac0` ends up at the correct 312.5 MHz afterwards. Harmless
  today, still worth silencing before a wide release. |

## Build trap: a changed base-files does NOT reach the image on its own

Editing anything under `target/linux/<t>/<sub>/base-files/` (or
`package/boot/uboot-tools/uboot-envtools/files/`) and then running
`make target/install` produces a **byte-identical image**. The package rebuilds
fine — `build_dir/target-*/linux-<t>_<sub>/base-files/.pkgdir/` shows the new
content — but the assembled rootfs staging dir
`build_dir/target-*/root-<target>` is *not* refreshed, so the image is built
from the old files.

This cost two full cycles here: the sysupgrade fix was "built" twice and the
sysupgrade.bin sha256 never changed from the broken one.

Do this instead:

    rm -rf build_dir/target-*/root-<target>
    make -j24 package/install target/install

`package/install` is required: it is what repopulates the staging root. Deleting
the directory and running only `target/install` fails with the unhelpful
`ERROR: target/linux failed to build.`

**Always diff the image sha256 before and after a base-files change.** If it did
not move, the change is not in the image, whatever the build log said. See also
[[feedback_validate_dont_trust_exit_code]] — exit 0 here means nothing.

## Release readiness (2026-09-09)

**Working and verified on hardware, installed to NAND:**

- Router mode end to end: `wan` DHCP lease, default route via `wan`,
  `ping 1.1.1.1` = 3.6 ms from the box.
- `lan1-3` in `br-lan` at 192.168.1.1; `lan3` links and forwards at 2.5 Gbps
  (0.8 ms RTT, ssh from a cabled host).
- `wan` at 1 Gbps, receiving and transmitting.
- Conduit `eth0` at 2.5 Gbps full, flow control rx/tx.
- Both wifi phys register and their cal blobs are extracted.
- `sysupgrade` works, and the firmware survives reboots (see the bootcount fix).

**Known gaps - be honest about these with testers:**

1. **No NSS offload.** Traffic that terminates on or is routed by the CPU is
   limited by the dual A53: ~293 Mbit/s single-stream, with `sys` time
   dominating. Port-to-port switching is done in the switch hardware and is
   unaffected, but **NAT throughput will be well below 2.5G**. Not yet measured
   properly - that needs a second cabled host.
2. **Two `rcg didn't update its configuration` warnings at boot**, both `mac0`,
   before the SerDes is up. `mac0` lands on the correct 312.5 MHz afterwards, so
   they are cosmetic today, but they should be silenced before a wide release.
3. **`lan1` and `lan2` have never had anything plugged into them.** They link-detect
   and are configured, but are untested with a real partner.
4. **Wifi radios register but association was never tested** - they are disabled
   by default like any OpenWrt build.
5. The switch-core reset and patch 0944 are **unproven** (see above); they are
   carried because the vendor does them.
6. Only **one unit** has ever run this. The second unit is still on stock.

## Experiment 2026-09-09: are the reset and patch 0944 actually needed?

Both were carried on "the vendor does it" grounds and explicitly flagged as
unproven, because both were originally tested against a driver that could not
write a register (the MDIO decode bug), so their apparent failure meant nothing.

**Test:** disabled `reset_control_bulk_reset()` in `qca8386_setup()` AND removed
patch 0944 from the patch set entirely. Built clean, RAM-booted.

**Result: no observable difference.**

```
FW_CTRL0 = 0x001004f0        identical to the working build
ping from a host on lan3     4/4, 0.755 ms
wan DHCP lease               10.0.0.57/24
ping 1.1.1.1 from the box    3/3, 3.48 ms
counters                     eth0 rx 12480, lan3 rx 6129, wan rx 6279
rcg warnings                 2, unchanged (the usual mac0 pair)
```

**NOT ACTED ON — the test has a hole.** This was a *warm* boot: the device had
already been running the working firmware, so the switch could have been carrying
configuration from the previous Linux boot that masks a genuinely required
init step. A cold power-on presents the driver with a fresh chip and is the case
that actually matters.

**Gate before deleting either:** power the device fully off at the wall, back on,
and confirm `FW_CTRL0` is still non-zero and forwarding still works with them
removed. That is a ten-second physical action and it is the only thing standing
between this and dropping ~40 lines of driver code, 13 DT reset phandles and a
whole patch. The code is restored and unchanged in the meantime.

## WIFI VALIDATED ON HARDWARE (2026-09-09) — the last big unknown is closed

Both radios were configured, enabled and a real client associated. Previously the
radios only *registered*; nothing had ever connected.

```
phy0-ap0   SPNMX57-LAB      2.4 GHz  ch 1   HE20   (IPQ5018 built-in)
phy1-ap0   SPNMX57-LAB-5G   5 GHz    ch 36  HE80   (QCN9074 on PCIe)
br-lan members: lan1 lan2 lan3 phy0-ap0 phy1-ap0
```

Client on the 5 GHz radio:

```
signal -36 dBm, last ack -31 dBm
tx/rx bitrate  1200.9 MBit/s  80MHz HE-MCS 11 HE-NSS 2
tx retries 0, tx failed 0
```

`HE-MCS 11 / NSS 2 / 80MHz` is **WiFi 6 at the full 2x2 ceiling for this chain** —
no fallback to legacy rates, no retries. Then end to end:

```
DHCP lease      192.168.4.231
ping from box   3/3, 6.8 ms
conntrack       425 flows from the client, NATed to the wan address
                e.g. 192.168.4.231:54853 -> 142.250.129.156:443
```

So **phone -> wifi -> br-lan -> CPU -> NAT -> wan -> internet works**. The
extender/router role is real, not theoretical.

Caveat that still stands: wifi traffic crosses the CPU regardless, because the
radios are not part of the DSA switch. Only wired-to-wired is hardware-switched.
Wireless throughput is therefore CPU-bound and **still unmeasured** — the 1200
Mbit/s figure above is the PHY rate, not achievable throughput. Do not quote it
as such.

## Measured throughput and the two settings that tripled it (2026-09-09)

First real performance data, iPhone speedtest over WiFi 6 (HE80, 2x2), 1 Gbit
uplink, device doing NAT:

| config | down | up |
|---|---|---|
| stock defaults | 164 | 385 |
| + software flow offloading | 257 | 444 |
| + RPS across both cores | **485** | **474** |

**+196% on download.** Note the baseline asymmetry (164 down vs 385 up) was the
tell: download is the harder direction because it makes the router *transmit* on
wifi and do the NAT work. Once both bottlenecks were removed the figures became
symmetric, which is what you would expect from a link that is no longer
CPU-starved.

### 1. Software flow offloading — persistent, in `/etc/config/firewall`

```
uci set firewall.@defaults[0].flow_offloading='1'
```

Was **off**. Shortcuts established connections past the full netfilter path.
Hardware offload (`flow_offloading_hw`) is NOT available - that needs NSS.
Caveat: flow offloading bypasses per-packet handling, so it is incompatible with
SQM/QoS shaping. If you ever want bufferbloat control here, they conflict.

### 2. RPS — `/etc/hotplug.d/net/20-packet-steering`

`IRQ 22 (eth0)` had **1,357,683 interrupts on cpu0 and exactly 0 on cpu1**: the
conduit is single-queue, so one core did every packet while the other idled.

**The uci option `network.globals.packet_steering` does nothing on this build.**
`netifd` contains no `rps_cpus` code and no `packet_steering` string at all, so
setting it is silently inert. Verified by grepping the binary. Hence the hotplug
script, which is now shipped in base-files.

RPS does not move the IRQ (that stays on cpu0, correctly) - it defers the softirq
processing to the other core, which is exactly the win.

### Still true

Wifi traffic crosses the CPU regardless; only wired-to-wired is hardware
switched. The 1200 Mbit/s in the station dump is a PHY rate, not throughput.
Wired-to-wired switching throughput is still unmeasured - it needs a second host.
