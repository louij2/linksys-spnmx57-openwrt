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
