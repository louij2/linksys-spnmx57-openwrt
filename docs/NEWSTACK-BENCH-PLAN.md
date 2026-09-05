# New-stack Phase 2b bench plan — first hardware probe test

**Goal of this bench session:** confirm the QCA8386 register-ACCESS premise on real
silicon — i.e. that the new `qca8386` DSA driver, over the indirect MDIO (page @
phy `0x18` reg `0x0c`), reads device id **`0x17`** from the switch at pseudo-PHY
`0x10`. This is the single fact the whole new-stack port rests on; everything
else in the driver is built on top of it.

This needs Luca + the board + UART. Up to here everything is build-verified only.

## The image
- `openwrt-qualcommax-ipq50xx-linksys_spnmx57-squashfs-sysupgrade.bin`
- md5 `8193aeda73f35af18d6961f6e1094755`
- On the build host: `srv-openstack:/tmp/owrt-main/bin/targets/qualcommax/ipq50xx/`
  (also an `-initramfs-uImage.itb` there for UART/tftp recovery boot)
- **⚠ /tmp is tmpfs (volatile).** Pull it to the Mac before a reboot of the host:

```bash
scp 'srv-openstack:/tmp/owrt-main/bin/targets/qualcommax/ipq50xx/openwrt-qualcommax-ipq50xx-linksys_spnmx57-squashfs-sysupgrade.bin' ~/Downloads/spnmx57-newstack-probetest.bin
```

## What this image is (and is NOT)
- It is the **new DSA stack** (kernel 6.18): wifi works, and it wires ONLY the
  QCA8386 switch + its CPU port (gmac1 @ 2500base-x SGMII+). No user-port
  ethernet yet (the four QCA8084 EPHYs need the nsscc + package DT nodes, Phase 2d).
- Flashing it **REPLACES the working v0.4.0** (old stack). Keep a v0.4.0 image to
  hand to roll back. UART backstop: boot the `-initramfs-uImage.itb` over
  tftp/UART if it won't come up. See `docs/flashing.md` / `docs/uart-findings.md`.

## Run it
1. Have UART open first: `/dev/cu.usbserial-110` 115200, root shell, no login.
2. Flash (from the box, over its wifi uplink, if reachable):
   `sysupgrade -n /tmp/spnmx57-newstack-probetest.bin`  (`-n` = don't keep config)
   or boot the initramfs itb over UART/tftp and sysupgrade from there.
3. After boot, the one command that matters:

```bash
dmesg | grep -i qca8386
```

## Decision tree
- **`detected QCA8386 (id 0x17 rev 0xNN)`** → 🎉 register access CONFIRMED. The
  page-`0x0c` + indirect-MDIO scheme and id `0x17` are correct. Next: spot-check
  the register MAP (is it really QCA8337/isisc-compatible?) before trusting the
  FDB/VLAN/PCS reuse — see the checklist. Then Phase 2c (CPU-port SerDes → traffic).
- **`Switch id detected 0xNN but expected 0x17`** → the read path works but the id
  differs. The access scheme is good; adjust the match value / re-check the
  datasheet. Still a big win (MDIO access proven).
- **No `qca8386` line at all** → the driver didn't bind or the read failed. Check:
  - `ls /sys/bus/mdio_bus/devices/` — did `switch@10` attach?
  - `dmesg | grep -iE "mdio|dsa|qca"` for errors
  - Is the switch really at 0x10 on this bus, and is gpio24 the right reset?
    Do a live MDIO scan (the old-stack `docs/mdio-scan.md` method) to re-confirm.
  - The page register may not be `0x0c` on the mgmt path — re-verify against the
    live bus / a trace.
- **Box doesn't boot** → UART recovery, boot the initramfs itb, reflash v0.4.0.
  The DT is a first iteration modelled on the working `ipq5018-spnmx56.dts`; the
  most likely culprits are the gmac1↔uniphy 2500base-x conduit or the CPU-port
  `fixed-link speed=<2500>` (swphy may reject 2500 — if so, drop to a phy-mode-only
  CPU port or `speed=<1000>` and re-test, since the probe read is independent of
  link state anyway).

## After access is confirmed
1. Spot-check a few QCA8337-family offsets (MASK_CTRL 0x000, PORT_STATUS 0x07c)
   to gauge register-map compatibility. A debugfs register-read knob can be added
   to `qca8386.c` if needed (the skeleton has none yet).
2. Work the `docs/phase2b-research/UNRESOLVED-CHECKLIST.md` HW items.
3. Phase 2c: implement the CPU-port SerDes bring-up (kit section 2) → traffic on
   the CPU port. Phase 2d: nsscc + qca8084-package DT + the 4 EPHY user ports.
