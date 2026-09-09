# SPNMX57 on the OpenWrt DSA stack

Board files and patches for the current-OpenWrt (kernel 6.18, mainline DSA
ethernet stack) port. **This is the shipping firmware** — validated on hardware
and running from NAND. The old `qca-ssdk` stack that produced v0.4.0 is
superseded.

Contents:

- `target/linux/qualcommax/dts/ipq5018-spnmx57.dts` — the board DTS: gmac1
  conduit at 2500base-x, the `qca8386` switch with lan1-3 + wan, the QCA8084 PHY
  package, and the nsscc clock/reset controller.
- `target/linux/qualcommax/files/drivers/net/dsa/qca/qca8386.c` — the DSA driver
  for the QCA8386 switch. Forked from mainline `qca8k`; no such driver existed
  anywhere before this.
- `target/linux/qualcommax/patches-6.18/0930-0944` — QCA8084 PHY package and
  nsscc support backported from qualcommbe, plus two fixes of our own (0943
  UNIPHY1 SerDes bring-up in switch mode, 0944 switch-core memory config).
- `target/linux/qualcommax/ipq50xx/base-files/` — board registration in every
  table that needs it (caldata, board.d, sysupgrade, uboot-envtools, bootcount,
  wifi-migrate) plus the RPS packet-steering hotplug script.
- `image-ipq50xx.mk.arm` — the `Device/linksys_spnmx57` recipe arm.
- `tools/ramboot-test.py` — non-destructive RAM-boot harness; TFTPs an initramfs
  and `bootm`s it, so nothing is written to NAND and a power cycle recovers.

Reuses `ipq-wifi-linksys_spnmx56` caldata and the `Linksys-SPNMX56` calibration
variant — the SPNMX57 shares the SPNMX56's ART layout, which the caldata fix
proved.

See `../docs/CONTINUE-HERE.md` for the full technical state and
`../docs/NEWSTACK-PORT-PLAN.md` for the original plan.

## Reproducing the tested firmware

The validated new-stack build was made against:

- OpenWrt main at `3809dfe5c69b43bc9b39f7b75fa493bbc2ad57ac` (version string
  `r36053-3809dfe5c6`)
- Kernel 6.18.44
- Feeds: the stock `feeds.conf.default` from that commit

Patches `0930-0944` in `target/linux/qualcommax/patches-6.18/` apply on top of
that base. Note the build tree used during development lived on tmpfs and is
volatile; everything needed to rebuild is mirrored here.
