# SPNMX57 on the new OpenWrt DSA stack (work in progress)

Phase 1 board files for the current-OpenWrt (kernel 6.18, new DSA ethernet
stack) port. See ../docs/NEWSTACK-PORT-PLAN.md for the full 6-phase plan.

- `target/linux/qualcommax/dts/ipq5018-spnmx57.dts` — Phase 1 DTS: wifi only,
  ethernet (QCA8386) disabled pending the new DSA driver.
- `image-ipq50xx.mk.arm` — the `Device/linksys_spnmx57` recipe arm to add to
  `target/linux/qualcommax/image/ipq50xx.mk` (after the spnmx56 arm).

Build tree lives at `srv-openstack:/tmp/owrt-main` (OpenWrt main + these files).
These copies exist because /tmp is not durable. Reuses `ipq-wifi-linksys_spnmx56`
caldata and `Linksys-SPNMX56` calibration variant (proven on the old stack).

Status: Phase 1 build in progress (first build = full toolchain, hours).
The shipped, working firmware is v0.4.0 on the OLD stack (branch main).
