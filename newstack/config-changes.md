# New-stack config changes (qualcommax)

Phase 2a validated (patches 0930-0941 apply + compile, drivers built into kernel):

- `target/linux/qualcommax/config-6.18`:
  `# CONFIG_IPQ_NSSCC_QCA8K is not set`  ->  `CONFIG_IPQ_NSSCC_QCA8K=y`
  (the QCA8386's internal NSS clock controller; drives the 4 QCA8084 EPHYs.
  Everything else the QCA8084 package needs was already =y:
  CONFIG_QCA808X_PHY, CONFIG_PHY_PACKAGE, CONFIG_QCOM_NET_PHYLIB,
  CONFIG_PCS_QCA_UNIPHY.)

Patches 0930-0941 = qualcommbe QCA8084 series 0301-0312, copied verbatim and
renumbered. Self-contained (no prereqs in qualcommbe 0001-0300) and
conflict-free (qualcommax patches none of the files they touch).

## Phase 2b (driver compile gate, validated)

- `target/linux/qualcommax/config-6.18`: add `CONFIG_NET_DSA_QCA8386=y`
- New driver: `files/drivers/net/dsa/qca/qca8386.c` (skeleton)
- New patch: `patches-6.18/0942-net-dsa-qca-add-qca8386-driver.patch` (Kconfig+Makefile)

Confirmed on the build host: patches apply, qca8386.o builds, EXIT=0, no
errors/warnings. This is the SKELETON (register access + probe + read_switch_id
0x17 + minimal DSA/phylink ops); FDB/VLAN/PCS body deliberately deferred to
Phase 2c/2d pending hardware confirmation of the register map.
