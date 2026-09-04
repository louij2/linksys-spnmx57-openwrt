# Handover: Linksys SPNMX57 OpenWrt Ethernet port

Repo: `~/Repositories/linksys-spnmx57-openwrt` -> `github.com/louij2/linksys-spnmx57-openwrt` (public)
Build host: `srv-openstack` (aarch64, 80 cores, 246 GB), tree `~/spnmx57/openwrt`, Docker image `owrt-deb12`.
**Ghidra runs on srv-openstack, never on the Mac.** Device is at **10.0.0.71**.

## Where things stand

Ethernet is most of the way up and does not yet pass packets. **The analogue
power-on theory has been tested and is dead** (2026-09-04, see below).

| | |
|---|---|
| QCA8386 switch detected and initialised by qca-ssdk | yes |
| Four QCA8084 EPHYs answer MDIO at addresses 1-4, PHY ID `0x004dd180` | yes |
| Registered as Linux `phy_device`s, bound to "QCA808X SSDK ethernet" | yes |
| `lan` netdev created by qca-nss-dp, up, carrier, in `br-lan` forwarding | yes |
| Runtime register access (`/sys/kernel/debug/qca8084/cmd`), no build needed | yes (2026-09-04) |
| PHY digital path proven: internal loopback links on every port | yes (2026-09-04) |
| Analogue trims: hardware efuse load honoured, software trims gated as the vendor gates them | yes (build 5, flashed) |
| **EPHY decodes anything from a live partner (LPA, page received)** | **no, on any port, ever** |
| **EPHY link with a live cable in a socket** | **no** |
| **Received packets** | **no — rx_packets 0, tx_packets climbing** |

Everything is currently configured for **1G**, deliberately. 2.5G comes after.

## 2026-09-04 EVENING: THE LINE WORKED THIS MORNING. THE DRIVER IS NOT THE REGRESSION.

Luca: the switch port's light was on this morning, before he left, with the
previous session's build (`287e5df`). So an EPHY **did** link then, invisibly
to every preinit sample (all taken before the ssdk runs), and `rx_packets = 0`
was a MAC-side problem, not a PHY one. Now, on the same box and cable, no port
links.

Bisected today, all with the knob's post-ssdk reads:
- Build 6 = **exactly this morning's preinit** (DTS `qcom,qca8084-software-trims`
  on, no `qcom,qca8084-afe-power-on`; boot line `boot choices: software trims
  ON, analogue block off`): still `LPA 0`, no page, no link at +45/+90 s.
- The ssdk's own hw_init runs every boot regardless (post-ssdk `0x8b80 = 0xf0`,
  BMCR `0x1040`), so today's driver additions never changed the post-ssdk state.
- Runtime on top of that: 1588_P2_EN cleared, software trims restored, ADC edge
  rising, hibernation off, MDI/MDIX forced: nothing.

**Identical software, different outcome, so the difference is state software
does not set.** Two candidates, both Luca's to test first:
1. **Reset history.** Every reboot today was warm (sysupgrade, plus the 07:44
   unbind crash). If the morning's link followed a real power-on and the
   package's analogue side does not recover from the GPIO reset pulse (preinit:
   10 ms assert, 50 ms settle; upstream `nss_cc_qca8k`: 100 ms hold), warm
   reboots leave exactly this: digital alive, line dead. Test: pull power 15 s,
   then read `c22 1 0x05/0x06/0x11` through the knob. Build 8 adds
   `preinit rst=<ms> post=<ms>`; **tried at runtime: 10/50, 10/100, 200/100 and
   2000/500 ms, no link after any of them**, so if a cold power-on restores it
   the recovery is not a longer pulse on gpio24 but something the pulse does
   not touch (a rail, or a second reset the vendor drives that we do not).
   **Confirmed 2026-09-04 evening: a full power cycle (power removed, not just
   reboot) does NOT restore it either** -- reg 0x05/0x06/0x11 identical to
   every warm-reboot reading. This rules out "the analogue side just needs a
   real power-on to recover"; reset depth from a 10 ms pulse up to removing
   power entirely makes no difference. The two live explanations are now:
   something physical changed since the morning (cable, socket, switch port),
   or the unit developed a fault in the intervening hours (marginal
connection, ESD, thermal).

   **Confirmed 2026-09-04 evening: also ruled out -- which physical socket.**
   The router's front panel has an "Internet" port and LAN1-3; the cable had
   been in LAN1 the whole day. `phy_address` mapping was confirmed by forced
   100FD mode, whose 0x0a/0x11 pattern reliably follows a real cable
   (`0x0a = 0x4000`, elevated `0x11`) regardless of which port it is in:
   **LAN1 = phy 1, Internet = phy 4**. Moving the cable to the Internet port
   reproduces the identical failure (`0x05 = 0x0000`, `0x06 = 0x0004`, no
   link) on a different socket, different PHY, same cable. So it is not one
   socket's magnetics or trace -- the fault is common across the switch's
   line side. **The loop-cable test (this box's own Internet port to LAN1) is
   the last remaining test that separates "this board" from "the cable /
   far-end switch / partner"**: if the two ends link to each other, the fault
   has been external all along; if not, it is inside this unit and no further
   register experiment will localise it further.

### RESOLVED 2026-09-04 evening: THE ROUTER'S LINE SIDE WORKS. THE FAULT WAS NEVER HERE.

The loop-cable test (Internet port / phy 4 to LAN1 / phy 1, one patch cable,
both ends on this box) **linked and stayed linked**:

```
BMSR 0x796d (autoneg complete, link up)  LPA 0x5c61 (real negotiated content)
EXP 0x0007 (page received)  STATUS 0x3c2c / 0x3c6c
```

Confirmed twice, 12 s apart, undisturbed. All day, with the intended external
switch, every port on every build showed `LPA = 0x0000` and `EXP` never set
the page-received bit -- zero signal exchange, ever. Here it linked within
seconds of connecting the cable. That is not a speed or capability mismatch;
it is the difference between total silence and a working handshake. **The
entire day's investigation (analogue power-on, calibration, reset timing,
bisecting the driver back to the exact preinit that worked this morning) was
chasing a problem that was never in this router.** The switch chip, all four
QCA8084 EPHYs, the analogue trims, the whole line side: all fine.

**Next step is physical, not software:** swap in the cable just proven good
by the loop test to reach the actual destination switch; if that links, the
original cable was bad. If not, try a different port on that switch. If
neither works, the fault is in the far-end switch/device, not this router.

This does not change anything else in this document -- the driver work,
the runtime knob, and the gated calibration are all real fixes worth
keeping regardless of where the fault turned out to be. It changes what to
do next: point every remaining effort at the cable/switch on the other end,
not at this box.
2. **Physical.** Same cable, same router socket, same switch port as this
   morning? The switch is not in Prometheus (no SwOS/EdgeRouter/RouterOS port
   changed state or went quiet in 14 h), so its history cannot be pulled.

## 2026-09-04 LATE: THE LOOPBACK "SUCCESS" WAS 10BASE-T. THE LINE SIDE IS DEGRADED, NOT HEALTHY.

Correcting the earlier read of the loopback result. `LPA = 0x5c61` decodes as
selector 802.3, **10BASE-T and 10BASE-T-FD only**, pause, ack -- and `0x11`
speed bits `[9:7]` were `000` = 10M. **Two 2.5G-capable PHYs, joined by inches
of cable, negotiated 10 Mbps.** That is not a working line side; it is the
slowest, most error-tolerant mode Ethernet has, achieved over the shortest
possible link. The correct reading of all the evidence is a severity gradient
with signal quality:

| partner | cable | result |
|---|---|---|
| own phy 1 <-> phy 4 | inches | scrapes 10BASE-T |
| MacBook 2.5G USB adapter | normal | FLP pages detected, codeword decodes `0xffff`, never completes |
| TP-Link switch | normal/longer | nothing at all, `LPA 0x0000`, no page ever |

**With the laptop attached (phy 3) the port gets further than any port ever got
with the TP-Link**: expansion reg bit 0 (LP is autoneg-able) and bit 1 (page
received) both set, so FLP bursts are genuinely arriving and being decoded --
into all-ones garbage. An all-ones link codeword is invalid by construction
(selector `11111`, every technology bit claimed, ACK and Remote Fault both
set), and BMSR bit 4 (Remote Fault) duly sets from it.

**Swept at runtime with the laptop attached, all with no effect on the
`0xffff` decode:** ICC and trim_a set to the hardware efuse values
(0x14 / 9 for phy 3); ADC clock edge rising vs falling (`debug 0x8b80`);
advertisement restricted to 10/100 base-page only, no 1000, no 2.5G, no
next-page (`LPA` then reads `0x0000` instead, still no link); forced 100M
full/half and 10M full/half with autoneg off (no link in any).

**Ruled out by direct measurement, not inference:**
- **Reference clock.** `/sys/kernel/debug/clk/clk_summary`: `cmn_pll` locked
  at 9.6 GHz, **`eth-50mhz` = 50000000 exactly**, `xo-clk` 48 MHz,
  `ref-96mhz-clk` 96 MHz. The switch's own divider (src 0 div 3 = 25 MHz from
  the 50 MHz XO) matches the vendor. The "both ends share a wrong clock, which
  is why only loopback works" theory is dead.
  *(Worth noting for robustness: `eth-50mhz` has enable_count 0 / prepare_count 0
  -- no Linux driver claims it, it runs only because the bootloader left it on.
  A future `clk_disable_unused` change upstream would silently kill this board.)*
- **The cable.** CDT on the live laptop cable reports all four pairs `NORMAL`
  (`0x8064 = 0x1111`), properly terminated.
- **Which socket / which PHY.** Reproduced on phy 1, 2, 3 and 4.

### THE DECISION POINT: hardware fault, or a config step this SKU needs?

Everything documented in the vendor's driver has been reproduced and gone
beyond, and the analogue side still only manages 10BASE-T over inches. Two
explanations remain and **no further register experiment separates them**:

1. **This unit has a hardware fault** (magnetics, line driver, ESD damage).
   Note the switch port light was on this morning and is not now, with
   identical software -- consistent with something degrading during the day.
2. **This SKU needs an init step nobody has.** The SKU fuse reads `0x265`,
   which is none of QCA8082/8084/8085/8386 that `qca_mht_sku_check()` knows.

**The test that separates them is stock firmware.** Flash the vendor image
back (or use the second SPNMX57, which has never been opened or reflashed)
and plug in a cable:
- **Stock links** -> the hardware is fine, our port is missing something, and
  the diff is worth finding.
- **Stock does not link either** -> hardware fault on this unit; move to the
  second unit and the port is probably fine as written.

Stock image: `http://download.linksys.com/updates/20250403t130837/FW_MX57CF_1.0.1.216553_prod.img`
md5 `62b76e25b194ecd42275460a7eedcace`.

## 2026-09-04 LATEST: THE BISECT WAS ANCHORED ON THE WRONG COMMIT

Luca pushed back ("I saw a flashing light this morning, why can't you fix it"),
which prompted reading the git log **with timestamps**. It reframes the day:

```
08:44  679a8c5  Ethernet works: lan interface up, bridged and forwarding
09:14  3fcf3a6  Port bitmaps for the SoC ESS, and drop the link to 1G for now
09:51  287e5df  Live PHY link sampling, UN-GATE CALIBRATION, leave the switch core RCG alone
```

Ethernet worked at **08:44** (`679a8c5`). The commit at **09:51** un-gated the
calibration, forcing the U-Boot software trim loop to run. Independently, later
the same day, that loop was proven to write **wrong values for this silicon**:
`trim_a 1/0/0/f` and `ICC 2/2/4/1` over the hardware efuse-loaded `8/9/9/6` and
`0x12/0x12/0x14/0x11`. Two independent lines of evidence converge on
`287e5df` as the regression.

**The bisect (build 6) was anchored on `287e5df`, not `679a8c5`** -- i.e. on a
commit that already contained the suspected break. Builds 6, 7 and 8 all
carried `qcom,qca8084-software-trims` in the DTS, forcing exactly the wrong
trims. So "build 6 reproduces this morning and still fails, therefore the
driver is not the regression" **was an invalid conclusion drawn from a
mis-anchored experiment.**

Nor did any earlier build test the right combination:

| build | calibration | analogue block | matches 08:44? |
|---|---|---|---|
| 1-4 | un-gated (wrong trims) | on | no |
| 5 | gated (correct) | **on** | no -- analogue block did not exist at 08:44 |
| 6-8 | **forced software trims** | off | no -- wrong trims |
| **UNIT2-CLEAN** | **gated (correct)** | **off** | **yes** |

`UNIT2-CLEAN` (no `qcom,qca8084-software-trims`, no `qcom,qca8084-afe-power-on`)
is the first image that reproduces the 08:44 configuration, plus the day's
genuine bug fixes and the debugfs knob.

**Generalisable lesson, and an expensive one: when bisecting against "it worked
at time T", get T from the commit log with timestamps and anchor on the commit
that was actually running then.** Anchoring on "the last commit before the
handover" silently included the regression in every arm of the experiment, and
produced a confident, wrong conclusion ("the driver is not the regression")
that redirected hours toward hardware and cabling.

## 2026-09-04 NIGHT: THE REGRESSION IS PROBABLY `3fcf3a6`, THE SGMII UPLINK DROP

The calibration theory above was **tested and is dead**. `UNIT2-CLEAN` (gated
calibration, hardware trims `8/9/9/6` / `0x12/0x12/0x14/0x11`, no analogue
block) was flashed to unit 1 and confirmed running by its boot line -- and
still no link on any port. So `287e5df` un-gating the calibration was not it.

Diffing the DTS between `679a8c5` (08:44, worked) and now points at the other
commit in that window, `3fcf3a6` (09:14, "drop the link to 1G for now"):

```diff
-  switch_mac_mode = <MAC_MODE_SGMII_PLUS>;      (both the SoC ESS and qca8386 nodes)
+  switch_mac_mode = <MAC_MODE_SGMII_CHANNEL0>;
-  forced-speed = <2500>;   port@0 (SoC ESS CPU port)
+  forced-speed = <1000>;
-  forced-speed = <2500>;   port@1 port_id=2  <-- MAC1, the actual SGMII uplink into the QCA8386
+  forced-speed = <1000>;
```

**Proposed mechanism, not yet confirmed:** the QCA8386's switch core clock is
sourced from `UNIPHY1_TX312P5M` (handover register map: `0x0c800004` RCG reads
src 1), and the vendor sets its rate with `UQXGMII_SPEED_2500M_CLK`
(`ssdk_mht_clk.c`). Dropping the uplink from SGMII+ 2.5G to SGMII 1G changes
that reference, so the switch core -- and the PHY subsystem timing hanging off
it -- would run at the wrong rate. Live reads support the premise: switch core
RCG `0x0c800004 = 0x00000101` = src 1 (UNIPHY1_TX312P5M), div 1.

**It also explains the one observation nothing else did: the loopback linking
at 10BASE-T.** Both EPHYs share the same (wrong) switch core clock, so they
agree with each other and scrape a link, while any external partner -- on a
correct clock -- cannot. Same reason the MacBook adapter's FLP bursts arrive
but every codeword decodes `0xffff`, and the TP-Link produces nothing at all.

**Under test:** current code (keeping the debugfs knob for observability) with
only `switch_mac_mode` reverted to `MAC_MODE_SGMII_PLUS` at both sites and both
`forced-speed` entries back to `2500`, matching `679a8c5`. The port bitmaps
added in the same commit are deliberately kept, since they fix a separate
MAC-side `rx_packets = 0` problem.

**Caveat, stated because two confident theories died tonight already:** this is
a hypothesis with a mechanism, not a confirmed cause. If reverting the uplink
does not restore link, the DTS is exonerated and the hardware/second-unit path
is where this goes.

## *** SOLVED 2026-09-04 22:10: THE REGRESSION WAS `3fcf3a6`, THE SGMII UPLINK DROP ***

Reverting `switch_mac_mode` to `MAC_MODE_SGMII_PLUS` at both sites and both
`forced-speed` entries to `2500` (matching `679a8c5`) **restored EPHY link
immediately**:

```
EPHY 3 post-ssdk +90s: BMSR 0x796d link UP  STATUS 0x3e4c  speed 2500M full   (MacBook 2.5G USB adapter)
EPHY 4 post-ssdk +90s: BMSR 0x796d link UP  STATUS 0xbd40  speed 1000M full   (TP-Link switch)
```

`LPA` reads valid codewords (`0xdde1`, `0xc1e1`, selector `00001`) instead of
the `0xffff` garbage / `0x0000` silence seen all day. Stable across three reads
5 s apart and the driver's own +45 s / +90 s samplers. **2.5G negotiated on the
laptop port**, so the port is not limited to 1G after all.

**Mechanism.** The QCA8386's switch core clock is sourced from
`UNIPHY1_TX312P5M` (RCG `0x0c800004` reads src 1), and the vendor sets its rate
with `UQXGMII_SPEED_2500M_CLK` (`ssdk_mht_clk.c`). Dropping the SoC-to-switch
SGMII uplink from SGMII+ 2.5G to SGMII 1G moved that reference, so the switch
core -- and the EPHY subsystem timing hanging off it -- ran at the wrong rate.
**Do not drop this uplink to 1G. It is not a "safe" simplification; it breaks
every front-panel port.**

It also explains the observation nothing else did: the loopback linking at
**10BASE-T**. Both EPHYs shared the same wrong switch core clock, so they agreed
with each other and scraped the most error-tolerant mode, while any external
partner on a correct clock could not decode at all.

### What this cost, and the lesson

Roughly eight hours went into analogue registers, calibration trims, reset
timings, reference clocks, cable and switch swaps, and a stock-firmware plan --
none of which was the cause. Two things would have found it in minutes:

1. **`git log` with timestamps, read against "it worked at time T".** The commit
   named "Ethernet works" was at 08:44; the break came in the *next two*
   commits. The bisect was anchored on `287e5df` (09:51) -- already broken --
   so every arm of the experiment contained the regression and produced the
   confident, wrong conclusion "the driver is not the regression".
2. **Diffing the DTS against the last-known-good commit**, which is where the
   change actually was. All the register spelunking was downstream of a
   two-line config change.

The prompt to do either came from Luca pushing back ("I saw a flashing light
this morning, why can't you fix it") rather than from the investigation.

### Still open (separate, pre-existing bug)

`rx_packets` remains 0 on `lan` while `tx_packets` climbs.
`adpt_mp_port_netdev_change_notify()` still logs `incorrect port 0`: the ssdk
derives the port from `dev->phydev`, which is the fixed-link software PHY on
`&dp2`, so `qca_ssdk_phydev_to_port()` returns 0, the bitmap check fails, and
it returns before `adpt_mp_port_rxmac_status_set(A_TRUE)`. The switch's own
port-to-port forwarding does not depend on this, so L2 traffic between front
panel ports should work; only the CPU port is affected.

## 2026-09-05: CPU-port RX -- two more theories dead, by source, before any build

- **Init ordering is fine.** `regi_init()` calls `ssdk_dt_parse()` (ssdk_init.c:2454)
  before `qca_scomphy_hw_init()` (2537) -> `qca_mp_hw_init()` ->
  `qca_mp_portctrl_hw_init()`. `forced-speed` + `forced-duplex` on the SoC
  ESS `port@1` (port_id 2) set `PHY_F_FORCE` via `hsl_port_force_speed_set()`
  (ssdk_dts.c:717-720) *before* portctrl init reads it, so port 2 takes the
  "forced" branch and has its RX MAC enabled at init (ssdk_mp.c:54). The
  `incorrect port 0` error from the netdev notifier is therefore noise: that
  path is not what enables RX here.
- **No speed mismatch on the SGMII+ link.** QCA8386 `PORT_STATUS` (0x07c +
  port*4) reads `0xfe` on port 0 = speed field 2. In
  `include/hsl/mht/mht_port_ctrl.h`: `MHT_PORT_SPEED_2500M` is `#define`d
  **equal to** `MHT_PORT_SPEED_1000M`. The register cannot distinguish 2.5G
  from 1G; the rate is set by the SerDes clock. `0xfe` is exactly what 2500
  looks like. Do not chase this again.

Remaining suspects, in order: (b) the QCA8386 not forwarding to its CPU port
(port lookup / port-VLAN membership); (c) frames not crossing the SGMII+ link;
(d) SoC MAC1 RX; (e) EDMA/nss-dp delivery. The QCA8386's per-port MIB
counters (readable over MDIO through the knob) split these: push frames out
of `lan`, watch port 0 RX (SoC->switch), port 3 RX (Mac's replies arriving),
port 0 TX (switch forwarding them back toward the SoC).

### 2026-09-05: LOCALISED. Both directions are dead across the SoC<->QCA8386 SGMII+ uplink.

QCA8386 per-port MIB counters (`0x1000 + port*0x100`, QCA833x layout: RX block
at +0x00..+0x50, TX block from +0x54), read through the knob:

| port | RX | TX |
|---|---|---|
| 0 (CPU port, faces the SoC) | **all 21 counters zero, always** -- no good byte, no bad byte, no FCS error, ever | thousands (TxBroad 3494, TxMulti 665, 443 KB in one window) |
| 3 (Mac) | real traffic | real traffic |
| 4 (TP-Link) | 9076 broadcasts in one window | real traffic |

- **switch -> SoC:** the switch floods thousands of frames out port 0 toward the
  SoC; `lan` `rx_packets` stays 0.
- **SoC -> switch:** `lan` `tx_packets` went 17 -> 27 during a `ping6 ff02::1`
  out of `lan`; port 0 RX stayed at zero.
- Port lookups include port 0 (`0x17`, `0x0f`) and FORWARD_CTRL1 floods to all
  ports (`0x7f7f7f`), so forwarding is not it. Both MACs read enabled.

**So both MACs are up, the switch works, and nothing crosses the SGMII+ lane in
either direction. That is a SerDes/PCS-layer fault on IPQ5018 UNIPHY1 <->
QCA8386 SRDS1**, not MAC enable, not forwarding, not the `incorrect port 0`
notifier (which is noise).

Caveats learned reading them: the MIB counters are **clear-on-read by the
ssdk's poll** (`fal_mib_cpukeep_set(A_FALSE)`), so values are per-window, not
cumulative -- compare zero/non-zero, not magnitudes. The uplink SerDes is
MDIO-addressable at 6 (SRDS1; `SERDES_CFG 0x1cc5`), with a vendor-specific
register layout (its "BMCR" reads 0x02ff). XPCS at 7 and SRDS0 at 5 read
all-ones and **that is expected**: the vendor asserts `MHT_UNIPHY_XPCS_RST` in
SGMII/SGMII+ mode, and SRDS0 is the unused port-5 uplink.

The SoC side (UNIPHY1 `0x98000`, ESS `0x39c00000`, GMAC `0x39d00000`) is
memory-mapped and was invisible: no devmem, no opkg, no ethtool. Build 9 adds
`mem <phys> [val]` to the knob (ioremap + readl/writel) to read it at runtime.
**Do not point it at an unclocked block; that can hang the bus.**

## *** 2026-09-05 00:40: CPU-RX ROOT CAUSE FOUND -- SGMII vs SGMII+ RATE MISMATCH ON THE UPLINK ***

The SoC<->QCA8386 uplink has its two ends configured for different SGMII
variants, so the serdes rates do not match and the link never establishes:

| end | register | mode |
|---|---|---|
| QCA8386 SRDS1 (MDIO addr 6) | MMD1 0x11b = **0x0820** | SGMII+ (bit11 0x800) + MAC (0x20) -> 2.5G/3.125G |
| SoC UNIPHY (phys 0x98000)   | MODE_CTRL 0x9846c = **0x0421** | sg_mode=1 (bit10), sgplus_mode=0 (bit11) -> plain SGMII 1G |

Read live through the build-9 `mem` knob. Both ends are otherwise healthy:
`OFFSET_CALIB_4 0x981e0 = 0xac1` (calib_done b7, pll_locked b6) on the SoC,
`CALIB4 0x78 = 0x0ac1` on the switch. But because the rates differ:
- SoC `CH0_IN_OUT_6 0x98488 = 0x60` -> ch0_link (bit7) = **0**
- SoC `LINK_DETECT 0x98570 = 0`
- switch SRDS1 `MMD26 r1 = 0` (SGMII channel status, no link)
- traffic probe: push frames out `lan`, switch port0 RXbroad `0x1000` stays 0.

This is the complete explanation for both-directions-dead. Not MAC enable,
not forwarding, not the `incorrect port 0` notifier.

**Why the SoC ended up in plain SGMII:** `_adpt_mp_port_interface_mode_set()`
(adpt_mp_portctrl.c ~960) returns early for a **forced** port
(`if (port_id==PORT1 || force_port) return SW_OK`) -- so it never reconfigures
the uniphy from the port config. The SoC uniphy mode is therefore whatever the
init path set it to, and that came out SGMII (1G), not SGMII+ (2.5G), despite
`&switch` (SoC ESS, device 0) carrying `switch_mac_mode = MAC_MODE_SGMII_PLUS`
and its `port@1` (port_id 2) `forced-speed = 2500`. So either nss-dp (which
logs "PHY Link up speed: 1000" and reads the `&dp2` `fixed-link speed = 1000`)
owns the SoC uniphy here, or the ssdk MP init maps `switch_mac_mode` to the
wrong PORT_WRAPPER mode. Resolving which is the next step -- and it means the
`&dp2` `fixed-link speed = <1000>` may NOT be cosmetic after all (the comment
there claims it is).

MP instance note: the SoC MAC1/port2 uplink is uniphy **instance 0** at phys
0x98000; the MP code only ever configures INSTANCE0, and `_adpt_mp_port_..._set`
is skipped for forced ports. All offsets used are < 0x800 so they sit in the
ess-uniphy@98000 window.

### 2026-09-05 01:15: fix target narrowed to the ssdk device-0 uniphy bring-up

- **nss-dp does NOT own the uniphy PCS mode.** `ip link set lan down`/`up` left
  SoC `MODE_CTRL 0x9846c` unchanged at `0x421` (sg_mode=1, sgplus=0). nss-dp
  logs "PHY Link up speed: 1000" but does not touch the sg/sgplus bits. So the
  SoC uniphy being plain SGMII is the ssdk's doing (or a reset default it never
  overrode), not nss-dp forcing 1G from the fixed-link. The `&dp2`
  `fixed-link speed=1000` is therefore probably cosmetic after all.
- **The ssdk manages BOTH devices**: dmesg "Initializing SCOMPHY Done!!"
  (device 0 = SoC MP, the MAC1 uplink path) and "Initializing MHT Done!!"
  (device 1 = QCA8386). Device 1 came up SGMII+ correctly; device 0 did not.
- **Enum values are NOT the bug**: `MAC_MODE_SGMII_PLUS` (dt-bindings) = 0xc =
  `PORT_WRAPPER_SGMII_PLUS` (ssdk enum). The parse stores switch_mac_mode raw,
  so device-0 mac_mode = 0xc = SGMII_PLUS, and `qca_mp_interface_mode_init()`
  passes that to `adpt_mp_uniphy_mode_set(dev0, INSTANCE0, 0xc)`.
- **But the register shows SGMII, or reset defaults**: `MODE_CTRL 0x9846c` =
  0x421 is what `adpt_mp_uniphy_mode_ctrl_set` writes for SGMII_CHANNEL0
  (sg_mode ENABLE), not SGMII_PLUS (which would set sgplus, ~0x821). And
  `MISC2_PHY_MODE 0x98218` = 0x70 has phy_mode field = 7, which is neither
  SGMII (3) nor SGMII+ (5) -- consistent with the uniphy config **never having
  run** on instance 0 and 0x98000 holding power-on defaults.

**Two possibilities remain, and they need one more careful trace (fresh, not
at 1am):**
1. The MP uniphy config errors or no-ops for instance 0 (e.g. `mode` retrieved
   as something other than 0xc at the call site, or `adpt_uniphy_mode_set`
   NULL / an early SW_NOT_SUPPORTED), leaving reset defaults.
2. **0x98000 is the wrong uniphy for MAC1.** IPQ5018 has two uniphy instances;
   MP `HPPE_UNIPHY_BASE1 = 0x10000`. If MAC1's uplink is instance 1
   (phys 0x98000+0x10000 = 0xA8000), then 0x98000 is MAC0/GE's uniphy (rightly
   SGMII) and I have been reading the wrong block. **Do NOT blind-read 0xA8000
   with the mem knob** -- an unclocked second instance can hang the bus and the
   box can't be power-cycled remotely until Luca is back. Confirm the instance
   from the ipq5018 dtsi / ssdk uniphy addressing FIRST, then read.

### FIX OPTIONS (for the next session, in rough order of cleanliness)
- **A. Make the ssdk actually set the SoC uniphy to SGMII+.** Find why
  instance-0 (or the correct instance) stays SGMII and fix the config path or
  the DTS property it reads. This is the root fix.
- **B. Force it via the knob after boot** to *prove* the fix before touching the
  config: add a knob command that re-runs the MP uniphy SGMII+ set (or writes
  MODE_CTRL sgplus + MISC2 phy_mode=5 + raw clock 312.5M + recalibrate). If
  link then comes up and rx flows, option A is confirmed as the target.
- **C.** If MAC1 truly needs 1G (SGMII) because 2.5G on the uplink is blocked by
  nss-dp/swphy, that conflicts with the EPHYs needing the switch at SGMII+
  (v0.3.0). So the uplink MUST be SGMII+ 2.5G on both ends; A/B are the path.

Debug tooling on the device now (build 9, `681b2ec3...`): `/sys/kernel/debug/qca8084/cmd`
gained `mem <phys> [val]` (ioremap+readl/writel). SoC uniphy instance 0 = phys 0x98000.

## THE CURRENT BLOCKER

The four EPHYs are alive digitally and dead on the line side. Sampled 6 seconds
after bring-up, well past autonegotiation, with a cable in:

```
EPHY 1..4 at +6s: BMCR 0x1040  BMSR 0x7949  link down | STATUS 0x0000 link down
```

(`0x1040` rather than the earlier `0x1140` only because preinit now soft-resets
each PHY inside hw_init and the reset clears the duplex bit; autoneg is still
on, so it is immaterial.)

### 2026-09-04: the analogue power-on was implemented, measured, and ruled out

Build `0919` pass 2 reads the analogue enables **before** writing them, after
every package reset in preinit and again after the hw_init soft reset. On a
cold boot, all four PHYs, cable in port 1 (or 4; both sampled):

```
phy 0x1..4 default, before hw_init: AFE25_CMN_6 0x963c PLL on | AFE25_CMN_2 0xf81a/0xf80a/0xf80a/0xf8fa LDO on
```

Bit 15 of `0x380` and bit 13 of `0x180` are **already set at reset**. After
hw_init, after the PLL write, after the LDO write: identical values. No link at
+6 s on any port. So `qca8084_phy_pll_on()` / `ldo_set()` are no-ops here, and
the reason the vendor never calls them at boot is that the silicon defaults
them on. **Retired.** The code stays in preinit because it is idempotent and
its reads are the only view of those registers, but it is not a lead.

Two things the same boot confirmed on the way: preinit's calibration writes
land (`0x180[7:4]` reads `1/0/0/f`, which is `trim_a` per port), and the
"as found" sample on a cold boot is a chip in reset (`no answer`), not data.

### 2026-09-04 later: what the runtime knob showed (post-ssdk, cable in port 1)

All four PHYs, read at 73 s uptime through `/sys/kernel/debug/qca8084/cmd`:

```
BMCR 0x1040  BMSR 0x7949  ANAR 0x1de1  LPA 0x0000  EXP 0x0004  1000BT-ctrl 0x0600  1000BT-stat 0x0000  0x11 0x0000
MMD7.0 0x3000  MMD7.1 0x0008  MMD7.32 0x0021 (phy1,2) / 0x00a1 (phy3,4)  MMD1.1 0x0002
WORK_MODE 0x10  EPHY_CFG 0x00020c41  SERDES_CFG 0x1cc5  0x0c800304 0x0  core CBCR 0x4221  core RCG 0x101 (src 1)
GEPHY sys CBCRs 0x0c8001b0..bc = 0x1 (on)   GMII TX/RX branches 0x058.. = 0x80000000 (off, expected pre-link)
```

- **LPA = 0 and EXP bit 1 (page received) = 0, on every port, across autoneg
  restarts, at +1/+5/+10 s.** The cabled PHY has never decoded a single FLP
  from the partner. This is not a capability mismatch; the PHY hears nothing.
- Forced 100FD and forced 10FD on port 1 and port 4: no link either. Under
  forced mode port 1 reads `0x11 = 0x2a00`, `0x0a = 0x4000` while port 4 reads
  `0x11 = 0x0040`, `0x0a = 0`, the first port-to-port difference ever seen, so
  port 1 is the cabled one. Note `0x11` on QCA808x is: link bit 10, duplex
  bit 13, **speed bits 9:7** (`0x380`: 0 = 10M, 0x80 = 100M, 0x100 = 1G,
  0x200 = 2.5G), MDIX bit 6. The sampler decodes speed from bits 15:14, which
  is the AR803x layout and wrong here; fix it in the next build.
- **CDT (vendor procedure, reg 22 <- 0x8400/0x8000, MMD3 0x8064..68) is
  inconsistent**: port 1 gives `0x4111` d0 `0xc800/0xd600/0x8700` in one mode
  and `0x1111` d0 0 in the other; empty port 2 gives `0x1111` (all pairs
  "normal") in one mode and `0x3333` (all "short") in the other. Empty sockets
  should read "open". Treat CDT as evidence the line side is not behaving,
  not as a cable map.
- **The ICC / trim writes are not the blocker either.** Efuse rows read
  `0x0c900048 = 0x00840000`, `0x60 = 0x10100000`, `0x68 = 0x20000000`,
  `0x5c = 0x0007c000`, giving raw ICC 2/2/4/1 per port, which is exactly what
  preinit wrote to `0x280[4:0]`. But efuse version (`0x0c900014` bits 23:16)
  is **8**, and `qca_mht_ethphy_icc_efuse_get()` inverts bit 4 for any version
  other than 1 or 2 (so 0x12/0x12/0x14/0x11), and the U-Boot gate
  `(value >> 16) - 1 < 2` is the same field, so **the vendor U-Boot skips the
  trim loop on this silicon and the vendor links with 0x180[7:4] / 0x280[4:0]
  at reset default**. Runtime sweep on port 1 with autoneg restart after each:
  ICC 0x02, 0x12, 0x1f, 0x00 and trim_a 0, 1, 0xf: LPA stays 0, no page, no
  link. Caveat: no soft reset between writes, only a restart-autoneg. A build
  that honours the gate (vendor behaviour) is still worth one boot, because it
  also reveals the reset defaults of 0x180/0x280, which nobody has ever read.

- **Internal loopback links; the line does not.** BMCR `0x4140` (the vendor's
  `QCAPHY_1000M_LOOPBACK`) and `0x6100` (100M) both give BMSR `0x794d` (link
  up) and `0x11 = 0x2e00` (link bit 10 set) within 3 s, on the cabled port
  and on an empty one. **PCS, DSP, clocking and the whole digital path are
  sound; the fault is beyond the loopback point: AFE, line driver, magnetics,
  or their supply.**
- Forcing MDI (`reg 16 [6:5] = 00`) or MDIX (`= 01`), each with a soft reset:
  no change, LPA 0, no page.
- Hibernation (debug `0x0b` bit 15, reads `0xbc80` = enabled on every PHY;
  the vendor never touches it at init): cleared on the cabled port, restart
  and soft reset, no change.

- **The U-Boot-transcribed software trims are wrong for this silicon, and
  they stick.** Build 4's `preinit nocal` (a knob re-run of the whole preinit
  without the trim loop; it did not crash the device, so the earlier crash was
  the unbind) reads, right after the `EPHY_CFG[21:20]` clear:
  `0x180[7:4] = 8/9/9/6`, `0x280[4:0] = 0x12/0x12/0x14/0x11`. The ICC values
  are exactly the ssdk's `raw ^ 0x10`, so the hardware loads the trims from
  efuse by itself (at reset release, not at the `[21:20]` clear: on a cold boot
  the software values `1/0/0/f`, `2/2/4/1` are still there after that clear).
  Build 5 honours the vendor's gate: no software trims unless efuse version is
  1 or 2, or `preinit cal` forces them. **With the hardware trims in place the
  cabled port still does not link** (LPA 0, no page), so this was a real
  defect but not the blocker.

**Everything reachable over MDIO now does what it is told, and nothing on the
line side responds.** The remaining candidates are things the PHY registers do
not control: a supply or enable for the package's analogue side that the
vendor drives from a GPIO/regulator, `EPHY_CFG` bits [21:20] which preinit
clears at the end of calibration without knowing their meaning, or a partner
that is not transmitting. Two hands-on tests split this in half for free:
the partner's link LED (dark = our transmitter silent; flapping = handshake
failing), and a patch cable from port 1 to port 2 of the same box (link
between two of our own PHYs proves the line side and blames the partner).

### 2026-09-04: an independent four-lens review of the vendor source reached the same wall

Four investigators (vendor U-Boot link-up path, ssdk boot path on this DTS,
vendor DTB vs ours, phylib/nss-dp interaction), each hypothesis attacked by
two skeptics. Eleven hypotheses were refuted against the source or against
the runtime reads above; the two survivors ("phylib/ssdk powers down or
isolates the EPHYs after 9.5 s", "advertisement never programmed because the
phy_devices are never attached") are both already contradicted by the knob
reads (BMCR 0x1040 at +45/+90 s; ANAR/1000BT/MMD7.32 sane; and no page ever
received, which no advertisement can fix). Refuted for the record, do not
re-derive: ICC bit-4 polarity; "state after 14 s is unmeasured" (it is now);
core/AHB unclocked post-ssdk (all CBCRs read enabled); SEC_CTRL clocks off
during fuse reads; MAC ports disabled until link poll (true, irrelevant to
PHY link); switch-core reset reverting WORK_MODE (reads 0x10 post-ssdk);
package reset pulse too short; 1000BASE-T master/slave fault; something
rewriting the EPHY cores after preinit.

**Every lens was a software-register lens, and the frontier is now the line
side.** What nobody has looked at, in the order to do it:

1. Partner LED and port-1-to-port-2 patch cable (transmitter silent vs
   handshake failing; partner in or out of the problem). Needs hands.
2. `preinit nocal` (build 4 knob) or a gate-honouring cold boot: hardware
   efuse-loaded `0x180/0x280` vs software trims, and the reset defaults.
3. Internal loopback (BMCR bit 14) and forced MDI/MDIX: PCS/DSP vs AFE, pair
   assignment vs everything else. Runtime, done below.
4. CDT as a partner-independent receiver test: a working QCA808x reads "open"
   on all four pairs of an empty socket; this one does not.
5. The package's own TLMM/GPIO block (the nsscc clocks a `tlmm` inside the
   QCA8386): a board-level rail or enable could be driven from there rather
   than from the SoC. `ssdk_mht_pinctrl_init` is the code that configures it;
   diff what it sets against the vendor U-Boot's pin setup.
6. Socket-to-PHY mapping and the 2x2.5G / 2x1G magnetics question.
7. Whether the stock U-Boot ever links with a cable in (UART, stock image).
   A unit-level hardware fault (magnetics, ESD-damaged line driver) is not
   excluded by anything so far; only this, or the loop cable, excludes it.

### THE NEXT THING TO DO: find what actually gates link

**Read the 2026-09-04 sections above first; they retire most of the list
below.** What is left, in order, and the first two need hands, not a build:

1. **Partner link LED** with the cable in port 1 (the leftmost RJ45 as seen
   from the back, if the socket order matches the PHY order; it is the one
   that reads differently in forced mode). Dark = our transmitter is silent.
   Flapping = we transmit and the failure is receive/decode.
2. **Patch cable from port 1 to port 2 of the same box**, then
   `echo "sample loop" > /sys/kernel/debug/qca8084/cmd` and read `c22 1 0x05`,
   `c22 1 0x06`, `c22 2 0x06`. Two of our own PHYs linking proves the line
   side and blames the partner/cable; both deaf to each other proves the
   fault is inside the package's analogue path or the board.
3. **Boot the stock image with the cable in** and watch the UART for the
   vendor U-Boot/Linux link line. This is the only thing that proves the
   sockets on this unit ever linked; a hardware fault (magnetics, ESD-damaged
   line driver) is not excluded by anything so far.
4. The package's own TLMM/GPIO block: `ssdk_mht_pinctrl_init` configures it;
   diff against the vendor U-Boot's pin setup for anything that could be a
   board-level enable on this design.
5. Socket-to-PHY mapping and which two sockets are 2.5G (magnetics differ).

Everything transcribed from U-Boot and from the ssdk's own PHY init is now
either verified as landing or verified as already the default, and none of it
gives link. The candidates have to come from what the vendor's *working*
system does that this one does not, or from what this system does that the
vendor's does not. Start from these, cheapest discriminator first:

1. **Is the vendor U-Boot's own link-up sequence being reproduced in full?**
   Preinit transcribes `FUN_4a94c630` (clocks, resets, calibration). The vendor
   U-Boot then *uses* the ports: it has an `ipq_qca8084_...` link-up / port
   enable path that runs after that routine and before Linux, and the vendor
   Linux ssdk inherits PHYs that are already linked. Diff that path against
   preinit in Ghidra on srv-openstack, looking specifically for writes on the
   EPHY's own MDIO address (BMCR / MMD7 autoneg / debug regs) rather than
   switch-side registers. The strings `ipq_qca8084_` in the APPSBL are the
   index.
2. **Does the vendor U-Boot leave the PHYs linked before Linux starts?** On
   the stock image, read the UART boot log for a link/speed line from U-Boot
   with a cable in. If it does, the missing step is in U-Boot's port bring-up,
   which narrows (1) a lot.
3. ~~`ssdk_sh`~~ There is no `qca-ssdk-shell` package anywhere in this tree
   (checked `package/` and `feeds/`; only `package/kernel/qca-ssdk`). Solved
   another way: the debugfs command file above.
4. **MMD7 autoneg advertisement and master/slave.** BMSR `0x7949` never sets
   bit 5 (autoneg complete). Read MMD7 `0x20` (1000BASE-T control / advert,
   via C45 like the vendor) and MII `0x09`/`0x0a` on the cabled port at +6 s,
   and compare with what a working QCA808x shows. A PHY advertising nothing
   valid, or forced master against a forced master, looks exactly like this.
5. **Partner-side evidence.** Whatever is on the far end of the cable: does
   *it* report link, link flaps, or nothing at all? A partner that sees no
   signal energy says the transmitter is off; one that flaps says the
   handshake is failing. This costs nothing and splits the space in half.

`BMCR 0x1140` = autoneg enabled, 1000/full, **not** powered down. `BMSR 0x7949`
advertises capabilities but never sets autoneg-complete or link.

**Former leading theory, now ruled out: the QCA8084 analogue front end is never
powered on.** Kept for the record of how it was reasoned and tested. The
vendor driver has explicit PLL and LDO enables that hang off port-enable /
PHY-power-on, which is driven by the phy being *started*. Nothing ever attaches
these phy_devices to a netdev — `lan` is bound to a fixed-link software PHY
instead — so that path never runs. That leaves the analogue side unpowered
while MDIO keeps working, which is exactly the symptom.

**These are plain MDIO debug-register writes and can go straight into preinit.**
The driver already has `qca8084_debug_write(bus, phy_addr, debug_reg, mask, set)`,
proven by the calibration code. From `src/hsl/phy/qca8084_phy.c` and
`include/hsl/phy/qca8084_phy.h`:

```
QCA8084_PHY_CONTROL_DEBUG_REGISTER0  0x1f    QCA8084_PHY_1588_P2_EN  0x0002
QCA8084_PHY_AFE25_CMN_6_MII          0x380   QCA8084_PHY_AFE25_PLL_EN 0x8000
QCA8084_PHY_AFE25_CMN_2_MII          0x180   QCA8084_PHY_AFE25_LDO_EN 0x2000
```

`qca8084_phy_pll_on(phy_addr)` is, per PHY:
```
debug 0x1f  |= 0x0002        (1588_P2_EN)
debug 0x380 |= 0x8000        (AFE25 PLL_EN)
mdelay(20)
```

`qca8084_phy_ldo_set(enable)` sets `debug 0x180 |= 0x2000` (LDO_EN) on **two**
PHYs only — `ephy_addr[1]` and `ephy_addr[2]`, i.e. the PHYs of mht ports 2 and
3, because LDO1/LDO2 are shared regulators driven from those two. It only does
this when all the *other* PHYs' PLLs are already on
(`qca8084_phy_pll_status_get` reads back `debug 0x380 & 0x8000`).

So the order to implement is: PLL on for all four, then LDO on for the middle
two, then re-sample link.

**2026-09-04, after checking the callers and not just the callee: the theory
above is not supported by the vendor source.** `qca8084_phy_pll_on()` and
`qca8084_phy_ldo_set()` are never called on the vendor's boot path. Their only
callers are the `FAL_ERP_ACTIVE` branch of `_mht_port_erp_power_mode_set()`
(`src/hsl/mht/mht_port_ctrl.c`), the resume half of an explicit userspace
low-power API (`fal_port_erp_power_mode_set`) whose entry half is the only thing
that ever turns those bits off. Port power-on for this chip is
`qcaphy_poweron()`, a BMCR bit-11 clear, and `0x1140` shows it already clear.
So on a vendor board the EPHYs link with PLL_EN/LDO_EN at their **reset
default** and no software writes them. The writes are implemented anyway, as an
idempotent experiment, but the build that carries them also **reads debug
`0x380`/`0x180` on all four PHYs before writing** (after every package reset,
and again after the hw_init soft reset). That read is the result that matters:
already set means the theory is dead; clear means the writes are the fix.

Also established by the same pass:
- **qca-ssdk already runs `qca8084_phy_hw_init()` on every cold boot** at
  ~11-14 s, from `ssdk_phy_driver_init()`, which is *before* `qca_mht_hw_init()`
  and its switch reset. So the CDT / ADC-edge / MSE "link fixes" have been
  applied to this silicon on every boot so far and did not give link. The
  preinit copy only adds anything on an unbind/rebind, where nothing re-runs
  the ssdk's PHY init.
- The per-port trims into debug `0x180[7:4]` / `0x280[4:0]` are **U-Boot
  only** (FUN_4a94c630). The vendor Linux driver never writes them; it only
  re-applies the cached ICC value to `0x280` on link transitions
  (`qca8084_phy_icc_fix_up`), and it derives that value with a bit-4 inversion
  depending on the efuse version (`mht_sec_ctrl.c` `qca_mht_ethphy_icc_efuse_get`)
  that the U-Boot transcription does not do. Worth remembering if 100M
  misbehaves later.
- The eight "isolate-clear" registers `0x0c800058..0x138` step `0x20` are the
  CBCRs of the four **GEPHY GMII TX/RX clock branches**
  (`ssdk_mht_clk.c`, `MHT_MAC1_GEPHY0_TX_CLK` at `0x58`, RX at `0x78`, ...).
  In switch mode the vendor never enables them at init; `mht_port_link_update()`
  enables a port's pair once its PHY reports link, pulses their reset and does
  the PHY FIFO reset. Not on the path to PHY link; needed for traffic after it,
  and that is the ssdk's job on link-up.
- **All-ones is not data.** This controller returns `0xffff` for a PHY that is
  not answering (in reset, or not at that address). It has every bit set, so
  an unguarded sampler prints "link UP, 2500M full, PLL on, LDO on" for a chip
  in reset. The "as found" sample at the top of preinit runs *before* the reset
  GPIO is deasserted, so on a cold boot it was always that. Sampler and
  debug-register RMW now treat `0xffff` as "no answer", as the vendor's
  `PHY_RTN_ON_READ_ERROR` does.

Also worth doing in the same build, all from `qca8084_phy_hw_init()`, all
commented in the vendor source as **link fixes**:
- `qca8084_phy_cdt_thresh_init()` — CDT thresholds
- `qca8084_phy_adc_edge_set(ADC_FALLING)` — "invert ADC clock edge as falling
  edge to fix link issue"
- MMD1 write `0x800a` = `0x51c6` — "configure signal energy detect threshold to
  fix link issue for some chips"
- `qca8084_phy_icc_init()`

## What is already ruled out

- **A write not landing.** Every write in the bring-up reads back correct.
- **The indirect protocol.** Fixed; see below. Proven bidirectionally.
- **The chip held in reset.** Fixed, and logged rather than assumed.
- **The CMN PLL unmanaged.** Fixed properly (patch 0069), it now locks.
- **Clock divider.** The GEPHY/SerDes sys RCG was already correct (src 0, div 3
  = 25 MHz from the 50 MHz XO) before we touched it. Verified by read-back.
- **Calibration.** Now un-gated and producing plausible distinct per-port trims
  (`0x1/0x2`, `0x0/0x2`, `0x0/0x4`, `0xf/0x1`). No link. Not the answer.
- **PHY driver binding.** All four bound to "QCA808X SSDK ethernet".
- **Switch forwarding / CPU port.** Not implicated: the PHYs themselves have no
  link, so nothing reaches the switch to forward.
- **1G vs 2.5G mismatch.** Dropping everything to 1G changed nothing.

## THE BUG THAT UNBLOCKED EVERYTHING

The 32-bit indirect register protocol was **dropping three address bits**:

```
bits [23:8]  page, written to phy 0x18 reg 0x0c
bits  [7:5]  select WHICH data pseudo-PHY: 0x10 | ((addr >> 5) & 7)
bits  [4:2]  the register pair within it, addr & 0x1c
```

The driver used a constant `phy 0x10`, so every access aliased into a 32-byte
window at the bottom of its page. Registers below offset `0x20` were correct by
luck, which is why parts of the sequence worked and made the addressing look
sound. Found by two measurements: a fuse-block dump repeating identically every
`0x20` bytes, and a write-mask probe showing `MEM_CTRL` writable only in
`[31:12]` — truncated, `0x44` is `0x04`, `GLOBAL_INTR_STATUS`, whose low bits
are read-only.

Fixing it made the EPHYs answer immediately.

## Two more sources, found 2026-09-04, both better than the bootloader

**1. Mainline already has this package's clock controller.** The 6.12 kernel in
this tree carries `drivers/clk/qcom/nsscc-qca8k.c` (`CONFIG_IPQ_NSSCC_QCA8K`,
off in `config-6.12`), Qualcomm's upstream clock/reset driver for the
QCA8084/QCA8386 NSSCC at `0x0c800000`, reached over MDIO with the same
page + `0x10|` pseudo-PHY indirect protocol this port had to rediscover. Its
tables independently confirm every offset preinit uses: switch core CBCR
`0x8` (RCG `0x0`), APB bridge `0x10`, AHB `0x170`, sys RCG `0x1a0`, SRDS0/1
sys `0x1a8/0x1ac`, GEPHY0-3 sys `0x1b0..0x1bc`, GEPHY GMII TX/RX branches
`0x58/0x78/0x98/0xb8/0xd8/0xf8/0x118/0x138`, every reset = CBCR bit 2. It also
has a `SWITCH_CORE_ARES` at `0xc` bit 2 that preinit never touches (the vendor
ssdk's table pulses bit 2 of `0x8` instead; both exist).

**2. The vendor's mainline-style QCA8084 PHY driver, unmerged, on patchew**
(v8, Luo Jie, 2023-12-15; lore is behind an Anubis wall, patchew is not):
`https://patchew.org/linux/20231215074005.26976-1-quic._5Fluoj@quicinc.com/`
Patches 09-13 are the whole package bring-up in C against the nsscc, and
patch 05/06 are the PHY init and link handling. Compared with preinit:

| upstream (`qca8084_probe` order) | preinit |
|---|---|
| set PHY addresses in `EPHY_CFG` (09) | same, `0x20c41` |
| `qca8084_clock_config` (11): SRDS0/1 sys 25 MHz, enable, assert both resets 20 ms, deassert; GEPHY0-3 sys enable, assert all four resets 20 ms, deassert; release `gephy0..3_soft` + `gephy_dsp` (= `0x304[4:0]`); **clear `QCA8084_EPHY_LDO_EN` in `EPHY_CFG`, commented "Enable efuse loading into analog circuit"**, 10 ms | same clocks/resets one at a time; `0x304[4:0]` cleared; **the `[21:20]` clear at the end of calibration is that same LDO_EN clear**, but preinit writes its U-Boot trims into `0x180/0x280` *before* it |
| `qca8084_common_clock_init` (12): APB 312.5 MHz, AHB 104.17 MHz, sec_ctrl/tlmm/cnoc/mdio/mdio_master AHB | left to the ssdk (`ssdk_mht_gcc_clock_init` does exactly this at 14 s) |
| work mode (13): `0xc90f030` mask `[5:0]`, switch = `BIT(4)` = `0x10` | same |
| `qca8084_config_init` (05): debug `0x8b80[7:4]` = 0xf (ADC edge), MMD1 `0x800a` = `0x51c6` | same (hw_init) |
| `link_change_notify` (06): reg `0x19` FIFO bits [1:0] clear, 50 ms, set; MMD7 `0x901d` IPG only for QXGMII | ssdk does the FIFO reset in `mht_port_link_update` |

The one semantic this adds: **on this silicon the analogue trims are loaded
from efuse by hardware when `EPHY_LDO_EN` is cleared.** That is why the vendor
U-Boot skips its software trim loop for efuse version 8, and why the ICC
inversion exists in the ssdk (it re-derives what hardware loaded). Preinit
clears the same bits, so the load should have happened; whether preinit's
earlier software writes to `0x180[7:4]`/`0x280[4:0]` interfere with it is
unknown and is the one remaining ordering difference on the analogue side.
A build that honours the gate (no software trims) and reads `0x180/0x280`
after the `[21:20]` clear answers it and also yields the reset defaults.

Nothing in either source powers, enables or configures the analogue front end
beyond the above. Autoneg advertisement and restart come from phylib's
`config_aneg` on the vendor side and from the ssdk's PHY init here.

## The other source that matters

**Stop reverse engineering the bootloader. qca-ssdk is the vendor's own Linux
driver and it ships in the build tree.**

```bash
ssh srv-openstack 'mkdir -p /tmp/ssdk && cd /tmp/ssdk && \
  tar --zstd -xf ~/spnmx57/openwrt/dl/qca-ssdk-2025.05.30~446db12b.tar.zst'
```

- `include/hsl/mht/mht_reg.h` — the entire QCA8084 register map, named
- `include/hsl/phy/qca8084_phy.h` — the PHY's own registers
- `src/hsl/phy/qca8084_phy.c` — PHY init, PLL, LDO, calibration
- `src/init/ssdk_mht_clk.c` — clock/reset table and how to drive it
- `src/init/ssdk_mht.c` — `qca_mht_hw_init`, the full bring-up order
- `src/init/ssdk_init.c` — `chip_ver_get` / `qca_detect_phyid`

It corrected two register names this port had guessed wrong and produced every
step that has worked since.

## Register map (from the vendor driver, not guessed)

Clock controller base `0x0c800000`. CBCR bit 0 = enable, bit 2 = clock reset,
bit 31 = status. Each clock's cfg register is at `base + rcg` (source select in
bits [10:8], divider `RCGR_HDIV` in bits [4:0]) and the **command register that
commits it is 4 bytes below**, bit 0 = update, hardware clears it when done.
Rate maths: `div = (prate * 2 / rate) - 1`.

| register | meaning |
|---|---|
| `0x0c800004` / `0x0c800008` | switch core clock RCG / CBCR (wants 312.5 MHz from src 1 = UNIPHY1_TX312P5M, which is not up early — leave it alone, the ssdk repoints it) |
| `0x0c8001a4` | RCG for the four GEPHY + two SerDes sys clocks (src 0 XO, div 3 = 25 MHz) |
| `0x0c8001a8` / `ac` | SRDS0 / SRDS1 sys clock branches |
| `0x0c8001b0`..`bc` | GEPHY0-3 sys clock branches |
| `0x0c800058`..`0x0c800158` step `0x20` | bit 0 cleared, eight registers |
| `0x0c800304` | bits 0-3 GEPHY P0-P3 MDC software reset, bit 4 DSP hw reset |
| `0x0c800308` | bit 0 global top-function reset (`MHT_GLOBAL_RST`) |
| `0x0c900000` | SKU fuse row — reads `0x265` |
| `0x0c900014` | fuse row (the "calibration gate") |
| `0x0c90f014` | `SERDES_CFG` |
| `0x0c90f018` | `EPHY_CFG`, four 5-bit PHY addresses at bits 0/5/10/15 |
| `0x0c90f030` | `WORK_MODE`, mode field bits [5:0] |
| `0x0c90f03c` / `40` | `MDIO_CTRL0` / `MDIO_CTRL1` |
| `0x0c90f044` / `48` | `MEM_CTRL` / `MEM_ACC_0` |

Work modes: `MHT_SWITCH_MODE 0x10`, `SWITCH_BYPASS_PORT5 0x20`,
`PHY_SGMII_UQXGMII 0x27`, `PHY_UQXGMII 0x2f`. Strap reads `0x3f`. The EPHYs
answer in **all** of them, so work mode is not what gates them.

**The SKU fuse reads `0x265`**, which is none of QCA8082 `0x1dc`, QCA8084
`0x1dd`, QCA8085 `0x1de` or QCA8386 `0x1df` — confirmed at the slowest MDIO
clock as well as the normal one, so not a clock artefact. This part is a variant
the vendor driver does not enumerate. It does not block anything
(`qca_mht_sku_switch_core_enabled()` returns false only for 8082/8084/8085) but
it means "this is a QCA8386" rests on the vendor device tree, not the silicon.

## What preinit does now, in order

1. Sample EPHY link "as found" (useful after a rebind, see below)
2. Deassert the package reset GPIO (tlmm gpio24, ACTIVE_LOW), `bus->reset()`
3. Pulse the global reset `0x0c800308` bit 0
4. Read `EPHY_CFG`, log strap addresses
5. Enable + reset-pulse the switch core CBCR (RCG left alone)
6. Commit the GEPHY/SerDes sys RCG (src 0, div 3)
7. Read SKU fuse at slow MDC, `MDIO_CTRL0/1`
8. `EPHY_CFG` low 20 bits <- `0x20c41` (addresses 1,2,3,4)
9. `SERDES_CFG` <- `0x1cc5` (5,6,7)
10. Enable + reset-pulse `0x0c8001a8`/`ac`, clear bit 0 across the eight
    `0x0c800058`.. registers, enable + reset-pulse `0x0c8001b0`..`bc`
11. Clear `0x0c800304` bits [4:0] (release MDC + DSP resets)
12. Per-port software calibration, **gated as the vendor gates it** since
    build 5 (efuse version 1 or 2 only; this silicon is 8, so it is skipped
    and the hardware-loaded trims stand). `preinit cal` on the knob forces it
    for experiments; `preinit nocal` forbids it
13. Clear `EPHY_CFG` bits [21:20], settle
14. `WORK_MODE` <- switch mode, then `MEM_CTRL`/`MEM_ACC_0` — **last, after
    every reset**, because the switch core reset pulse reverts work mode
15. **Analogue front end power-on** (new, 2026-09-04): per PHY, the three
    `qca8084_phy_hw_init()` link fixes (eight CDT thresholds over MMD3, ADC
    clock edge falling on debug `0x8b80` followed by the BMCR soft reset the
    vendor does there, MMD1 `0x800a` = `0x51c6`), then `qca8084_phy_pll_on()`
    on all four (debug `0x1f` |= `0x0002`, debug `0x380` |= `0x8000`, 20 ms),
    then LDO_EN (debug `0x180` |= `0x2000`) on the PHYs of ports 2 and 3 only,
    gated on all four PLLs reading back on, as the vendor gates it. MMD goes
    over Clause 45 (`bus->write_c45`) because that is the path the vendor
    uses (`is_c45 = A_TRUE`), not the Clause 22 indirect form
16. Sample EPHY link post-init, +3s, +6s, now with `AFE25_CMN_6` (PLL) and
    `AFE25_CMN_2` (LDO) read back alongside BMCR/BMSR; full 32-address bus
    scan. After an unbind/rebind the "as found" pass shows whether qca-ssdk
    left the PLL/LDO enables standing

## Runtime register access: `/sys/kernel/debug/qca8084/cmd`

Added 2026-09-04 (patch 0919 pass 3). The mdio driver exposes the accessors
preinit uses, on the live bus under `bus->mdio_lock`, as one debugfs command
file. This replaces the build-flash-dmesg loop for anything on the MDIO bus.

```
echo "c22 1 0x01"          > /sys/kernel/debug/qca8084/cmd   # BMSR of phy 1
echo "c22 1 0x00 0x1200"   > .../cmd                          # write BMCR, read back
echo "c45 1 7 0x20"        > .../cmd                          # MMD7 reg 0x20 (Clause 45)
echo "dbg 1 0x380"         > .../cmd                          # debug reg via 0x1d/0x1e
echo "dbg 1 0x380 0x963c"  > .../cmd                          # write whole debug reg
echo "sw 0x0c8001b0"       > .../cmd                          # 32-bit indirect switch reg
echo "sw 0x0c8001b0 0x1"   > .../cmd                          # write, read back
echo "sample after ifup"   > .../cmd                          # sampler to dmesg, labelled
echo "scan"                > .../cmd                          # PHY ID at all 32 addresses
echo "preinit"             > .../cmd                          # re-run the WHOLE preinit (7 s, holds the bus lock)
echo "preinit nocal"       > .../cmd                          # same, software trim loop forbidden
echo "preinit cal"         > .../cmd                          # same, software trim loop forced (vendor never does on this silicon)
cat /sys/kernel/debug/qca8084/cmd                             # result of the last command
```

`preinit` re-pulses the package reset and every clock/reset in the sequence,
then restores WORK_MODE/MEM_CTRL, so the switch-side state the ssdk set up
(port VLANs, MAC clocks) is gone until reboot; fine for PHY-link experiments,
not for traffic tests. It logs `0x180/0x280` per PHY immediately after the
`EPHY_CFG[21:20]` clear, which is the hardware efuse-load trigger, so
`preinit nocal` shows the values the hardware loads with no software trims in
the way (the vendor's state on this silicon), and `preinit` shows whether the
software trims survive that trigger.

Numbers take any base `%i` does. Errors come back as the write's errno and
`cat` says which op failed. The driver also re-runs the sampler by itself at
**45 s and 90 s after the bus registered** (`post-ssdk +45s` / `+90s` in
dmesg), which is the first view this port has ever had of the PHYs after
qca-ssdk, qca-nss-dp and netifd have all run.

## The free experiment loop -- NOT FREE ANY MORE

**2026-09-04: with the current firmware the unbind crashed the device.**
Issued at uptime 16654 s on a cold boot of `287e5df` (four real
`ethernet-phy@1..4` nodes bound to "QCA808X SSDK ethernet", `lan` up in
`br-lan`); the write to `unbind` never returned and the next ssh found uptime
at 223 s. No pstore. Whether it is the unbind (tearing down phy_devices the
ssdk holds) or the re-run of preinit under a configured switch is not known;
either way the loop that used to be free now costs a reboot and a UART capture
would be the way to see why. Cold boot is the only measurement path until that
is understood. The description below is kept because it was true before the
PHYs were registered as phy_devices, and may be again if that is what broke it.

Unbind and rebind re-runs the whole of preinit at runtime, no build:

```bash
ssh root@10.0.0.71 'echo 90000.mdio > /sys/bus/platform/drivers/ipq4019-mdio/unbind
                    echo 90000.mdio > /sys/bus/platform/drivers/ipq4019-mdio/bind'
dmesg | tail -60
```

The "as found" sample at the top of preinit then reports the state the previous
session settled into, which is the only runtime view of PHY link this port has.

## Device tree, current state

- `&switch` (SoC ESS, device 0): `MAC_MODE_SGMII_CHANNEL0`, port@0 = port_id 1
  with mdio0 phy 7 (required for chip detection), port@1 = port_id 2 forced 1000.
  **`switch_cpu_bmp = <0>`, `switch_lan_bmp = <(ESS_PORT1 | ESS_PORT2)>`,
  `switch_wan_bmp = <0>`** — all three must be present or none are read, and
  without them `adpt_mp_port_netdev_change_notify()` bails before enabling the
  RX MAC.
- `&switch1` (QCA8386, device 1): `MAC_MODE_SGMII_CHANNEL0`, mac_mode1 disabled,
  cpu_bmp ESS_PORT0, lan_bmp ports 1-4, phy_address 1,2,3,4, port@0 forced 1000.
- `&mdio1`: `qcom,qca8084-preinit`, `qcom,qca8084-preinit-debug` (verbose; drop
  for a release), `qca8084-reset-gpios` (deliberately NOT `reset-gpios`), and
  four real `ethernet-phy@1..4` nodes.
- `&dp2`: `status okay`, `label = "lan"`, `fixed-link { speed 1000; full-duplex }`.
- `&mdio0`: must stay enabled or ssdk aborts.

Config additions in `config/qualcommax-config-additions`: `CONFIG_IPQ_CMN_PLL=y`
(needs patch 0069), `kmod-qca-ssdk`, `kmod-qca-nss-dp`.

## Known loose ends

- **Wi-Fi calibration variant is the wrong board.** `dts/ipq5018-spnmx57.dts`
  carries `qcom,ath11k-calibration-variant = "Linksys-SPNMX56"` in both places
  (lines 333 and 391). Fix after 1G flows. Before switching the string, confirm
  the new one exists inside the ath11k `board-2.bin` the image ships
  (`strings board-2.bin | grep -i spnmx`); ath11k matches this string against
  the BDF and a variant that is not there means no board data is loaded for
  this board at all, which is worse than the neighbour's.
- Generated bridge config lists ports `lan1 lan2 lan3`, which do not exist; the
  interface is `lan`. Set by hand on the device; needs fixing in board.d.
- `adpt_mp_port_netdev_change_notify: incorrect port 0` — the ssdk derives the
  port from `dev->phydev`, which is our fixed-link software PHY, so
  `qca_ssdk_phydev_to_port()` returns 0.
- **2.5G is blocked**: nss-dp always ends up with a phy_node (falls back to the
  dp node itself), so a fixed-link is mandatory, and `swphy_decode_speed()` only
  knows 10/100/1000 because Clause 22 cannot encode 2500. Restoring 2.5G means
  patching swphy or making nss-dp take its speed from the ssdk.
- Patch 0069 (CMN PLL never buildable) is an upstream OpenWrt bug affecting every
  IPQ5018 board and should be reported.

## Build / flash / test loop

```bash
# full build, ~45 min
ssh srv-openstack 'cd ~/spnmx57/openwrt && \
  rm -rf build_dir/target-aarch64_cortex-a53_musl/linux-qualcommax_ipq50xx/linux-6.12.57 && \
  rm -f bin/targets/qualcommax/ipq50xx/*.bin && \
  nohup docker run --rm -u 1000 -v ~/spnmx57/openwrt:/build -w /build owrt-deb12 \
    nice -n 15 make -j24 > /tmp/buildN.log 2>&1 < /dev/null & disown'
```

Compile-check first with `make target/linux/compile V=sc -j24`; `V=sc` is the
only way to see a compiler error.

**Patch generation is the biggest time sink here.** Always diff the mdio patch
against `/tmp/mdio-ipq4019-PRISTINE.c` on srv-openstack, never against
`build_dir` (which already has the patch applied), and always re-apply the
result to a fresh pristine copy to prove it round-trips. `/tmp/wire_cmn3.py` has
a hard guard that refuses to build a diff from a contaminated base — copy that
pattern. The working files live in `/tmp/work/{a,b}` on srv-openstack.

**Flash as separate commands, never chained**, and **the first flash often does
not take — check and repeat**:

```bash
scp -O build.bin root@10.0.0.71:/tmp/spnmx57.bin
ssh root@10.0.0.71 'sysupgrade -T -F /tmp/spnmx57.bin'
ssh root@10.0.0.71 'setsid sysupgrade -F -v /tmp/spnmx57.bin >/dev/null 2>&1 &'
```

Then **wait for uptime to drop** before reading dmesg, and confirm the new code
is live by a log line only it emits. `dmesg` right after issuing a flash shows
the *old* boot and reads exactly like a failed fix. This cost real time twice.

`CONFIG_DEVMEM=n`, no busybox `devmem`, no mdio-tools, so there is no userspace
path to these registers. Everything goes through the driver.

## UART

`/dev/cu.usbserial-110`, 115200, on the Mac. `screen /dev/cu.usbserial-110 115200`.
macOS screen is 4.00.03 with no `-Logfile`; use `-L`, it writes `screenlog.0` in
the cwd. A plain `cat` of the device gives garbage because opening it resets the
line to 9600 — hold the fd open across the `stty` if scripting it.

## Releases

- `v0.1.0-wifi-only` — everything but Ethernet, usable as a wireless repeater
- `v0.2.0-ethernet-bringup` — switch and PHYs up, no traffic yet
- (uncut) 2026-09-04 patch 0919 passes 1-5: analogue power-on measured and
  retired, all-ones guards, debugfs knob + post-ssdk re-samples, re-runnable
  preinit, calibration gated as the vendor gates it, reg 0x11 speed decode

Luca wants, in order: **1G traffic end to end**, then a full write-up for the
OpenWrt forum (someone has asked) and to share the DTS, then 2.5G.
The repo goes private until 1G flows.
