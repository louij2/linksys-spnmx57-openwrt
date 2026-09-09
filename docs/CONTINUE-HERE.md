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
- 🔧 **Forwarding: root cause found, fix built.** Links are up but no frames
  traverse - `eth0` (conduit) `rx_packets=0`, not even ARP. The same cable does a
  13MB TFTP at 2.5G under U-Boot, so the physical path is fine. The switch core
  reads back all zeros; see "the forwarding blocker" for the cause (switch-core
  memory config, patch 0944) and for what has already been ruled out.
- ⏳ then: router mode config, NAND flash, release

## The wifi fix (committed)

`linksys,spnmx57` was missing from the ipq50xx `11-ath11k-caldata` hotplug
script - the only thing that carves per-device cal out of the `0:art` partition
into `/lib/firmware`. Without it ath11k fails with `failed to load board data
file: -12` and registers no phys. Adding spnmx57 to spnmx56's case arms
(`0:art @ 0x1000` built-in, `@ 0x26800` QCN9074) fixes it: `/sys/class/ieee80211`
now shows phy0+phy1 and both `cal-*.bin` are extracted. This also proves
spnmx57 shares spnmx56's ART layout.

## The forwarding blocker — root cause found (fix built, not yet HW-proven)

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
