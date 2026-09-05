# CONTINUE HERE — SPNMX57 new-stack port (LIVE handoff 2026-09-05)

Session moved off the Mac (offline) to tower. **Memory does NOT sync between
machines — THIS FILE + docs/ + the repo are your context.** Also read:
NEWSTACK-PORT-PLAN.md, NEWSTACK-QCA8386-DSA-DESIGN.md, NEWSTACK-PHASE2B-KIT.md,
phase2b-research/UNRESOLVED-CHECKLIST.md, NEWSTACK-BENCH-PLAN.md, HANDOVER.md.

## Current live state
- Old-stack **v0.4.0 is the WORKING firmware; the box is on it now** (ethernet
  fine: EPHY3 2500M, EPHY4 1000M).
- New-stack port branch `newstack-port`: Phase 1 (wifi boot), 2a (QCA8084+nsscc
  deps), 2b (`qca8386.c` DSA driver skeleton) ALL BUILD CLEAN.
- We flashed the new-stack probe-test image with `sysupgrade -n`. **IT DID NOT
  BOOT** — the Linksys A/B dual-partition failsafe reverted to v0.4.0, so we
  never saw our driver's `detected QCA8386 (id 0x17)` line.
- **IMMEDIATE TASK:** capture the new-stack image's SERIAL BOOT LOG to find why
  it fails to boot (panic vs hang vs our qca8386 probe), fix, rebuild, retest.

## Access map (all via Tailscale)
- **Ayaneo = UART bridge to the Linksys box:** `ssh luca@100.109.80.34`
  (Windows, key id_ed25519). Box UART = **COM1, 115200**. Drive it via
  PowerShell `System.IO.Ports.SerialPort` (read AND write, to send the box
  console / U-Boot commands).
- **Build host:** `ssh srv-openstack`. New-stack tree `/tmp/owrt-main`
  (TMPFS-VOLATILE). Build container: docker `owrt-build` (80 cores). Persistent
  built images: `srv-openstack:~/spnmx57-newstack-artifacts/` (sysupgrade md5
  `8193aeda73f35af18d6961f6e1094755`).
- **Box:** serial-only now (wifi-STA down). Was 10.0.0.195.
- Repo: GitHub `louij2/linksys-spnmx57-openwrt` (PRIVATE), branch `newstack-port`.

## Build-host trap
Run `make defconfig` INSIDE the `owrt-build` container, never on the host —
srv-openstack's system python is 3.14 and bakes a dangling
`staging_dir/host/bin/python3` symlink; the container has python3.11. Fix:
`ln -sf /usr/bin/python3.11 staging_dir/host/bin/python3`.

## To capture the boot log / retest loop
Image is on srv-openstack:~/spnmx57-newstack-artifacts/ (+ Mac backup, +
volatile /tmp). Push to the box: `ssh root@<box> "cat > /tmp/img.bin" < img.bin`
(dropbear has NO sftp — do NOT plain scp; use `scp -O` or the cat pipe). Then
over the ayaneo COM1 serial: `sysupgrade -n /tmp/img.bin`, and WATCH COM1
through the (failed) boot to capture the panic/hang. The failsafe reverts to
v0.4.0, so this loop is safe.
Likely boot-failure suspects: CPU-port `fixed-link speed=<2500>` (swphy may
reject 2500 → try phy-mode-only, or speed=1000), or the gmac1/uniphy
2500base-x conduit bring-up. Driver+DT live in `newstack/` (mirror) and in the
build tree under target/linux/qualcommax/{files,dts,patches-6.18}.

## Backups (nothing lost)
- srv-openstack:~/spnmx57-newstack-artifacts/ (new-stack images, persistent).
- Mac ~/spnmx57-firmware-backup/ (v0.4.0 bit-identical + new-stack + 11 debug
  builds) — offline while the Mac is away.
- GitHub: all source, `v0.4.0-cpu-rx` release, repo `images/`.

## Rollback
v0.4.0 sysupgrade sha256 `6c951301ad56cf9382bac67025d4e56042dce6903d36ba36eb9ca81418c41c45`
(repo `images/openwrt-...-squashfs-sysupgrade.bin`, or Mac backup). The box
auto-reverts on boot failure regardless.
