# Docs index

Start with the [main README](../README.md) for what this is and current
status. Everything below is organised by what you're trying to do.

## Using it

- **[flashing.md](flashing.md)** — how to actually flash it: stock → OpenWrt
  (proven, no UART), `sysupgrade`, the dual-partition recovery net, and UART
  as a last resort.
- **[hardware.md](hardware.md)** — the board as described by the vendor's own
  device tree: SoC, switch, radios, buttons, LEDs.
- **[build.md](build.md)** — building the image yourself, including the
  vendor-SSDK-vs-mainline-DSA decision and the build-host gotchas.

## How it works — the porting story

Read in this order if you want the full technical narrative, not just the
result:

1. **[investigation.md](investigation.md)** — root-causing the `-22` probe
   error: what the QCA8386/QCA8084 package actually is, and why mainline
   didn't recognise it.
2. **[ethernet-bringup-log.md](ethernet-bringup-log.md)** — the full
   debugging journal for the original vendor-qca-ssdk port: the line-side
   dead ends, the CPU-RX fix, the SGMII-vs-SGMII+ rate mismatch that broke
   every front port. Long, dead-ends included, on purpose — this is the one
   to read if you want to see the debugging method, not just the answer.
3. **[newstack-porting-log.md](newstack-porting-log.md)** — the mainline DSA
   rewrite that replaced the above and ships today: the QCA8386 driver
   written from scratch, the MDIO register-decode bug that cost the most
   time, the Wi-Fi and conduit fixes, and the performance work.

Supporting design/reference material for the DSA rewrite:

- [NEWSTACK-QCA8386-DSA-DESIGN.md](NEWSTACK-QCA8386-DSA-DESIGN.md) — the
  driver design written before implementation.
- [phase2b-research/](phase2b-research/) — register maps and fork-mapping
  notes gathered while writing the driver (MHT register map, CPU-uplink MMD
  sequence, device-tree clock nodes, the `qca8k` fork map).
- [uboot-qca8084-protocol.md](uboot-qca8084-protocol.md) — the vendor
  U-Boot's own QCA8084 access, reverse-engineered from a live device.
- [mdio-scan.md](mdio-scan.md), [uart-findings.md](uart-findings.md) — raw
  bus scans and serial captures from early bring-up.
- [NSS-OFFLOAD-ASSESSMENT.md](NSS-OFFLOAD-ASSESSMENT.md) — why there's no
  NSS hardware offload on this stack, and what it would take.

## Development log

Dated, narrower-scope notes from along the way — useful if you're curious how
a specific milestone was reached, not required reading:

[first-boot.md](first-boot.md), [lab-poc.md](lab-poc.md),
[flashing-lab-notes.md](flashing-lab-notes.md) (superseded by flashing.md —
kept for provenance, not a procedure to follow), [bench-test.md](bench-test.md),
[preinit-port.md](preinit-port.md),
[preinit-remaining-scope.md](preinit-remaining-scope.md),
[NEWSTACK-PORT-PLAN.md](NEWSTACK-PORT-PLAN.md),
[NEWSTACK-PHASE2B-KIT.md](NEWSTACK-PHASE2B-KIT.md),
[NEWSTACK-BENCH-PLAN.md](NEWSTACK-BENCH-PLAN.md),
[phase2b-research/UNRESOLVED-CHECKLIST.md](phase2b-research/UNRESOLVED-CHECKLIST.md).

[roadmap.md](roadmap.md) is the original project scoping from before Ethernet
was solved — historical, superseded by the main README's status table.

## Upstream

- [upstream/forum-facts-sheet.md](upstream/forum-facts-sheet.md) — verified
  facts and numbers for posting to the OpenWrt forum, **in your own words**.
  The forum removes AI-generated posts; this file is reference data only, not
  a draft to paste.
