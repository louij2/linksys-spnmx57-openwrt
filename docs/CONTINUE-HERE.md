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
- ✅ **Phase 2c — CPU-port conduit at 2.5G** (the crux; see "the conduit fix")
- ⏳ Phase 2d — the four QCA8084 user ports (lan1-4). **Gate: the nsscc parent
  clocks have no in-tree provider.**
- ⏳ wifi caldata (`-12` board-data failure, no radios), router mode, NAND flash

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
