# The QCA8084 preinit: what's confirmed working, and what's actually left

Written 2026-09-03 after the first live test of the read-only preinit patch.
Read this alongside `docs/uboot-qca8084-protocol.md`.

## Confirmed working, on real hardware

The `qcom,qca8084-preinit` patch (read-only: reads `EPHY_CFG`, logs it, writes
nothing) built, flashed, booted, and produced:

```
qca8084: EPHY_CFG = 0x00318820
qca8084: port 0 strap addr = 0x0
qca8084: port 1 strap addr = 0x1
qca8084: port 2 strap addr = 0x2
qca8084: port 3 strap addr = 0x3
```

Two things this settles:

1. **The read protocol works.** A page-select write followed by two 16-bit
   reads at a fixed pseudo-address, exactly as decompiled, produced a clean,
   sensible, non-garbage 32-bit value on the first real attempt
2. **The strap addresses are 0-indexed: `0,1,2,3`, not `1,2,3,4`.** The
   vendor's own DTS declares `phy_address = <1>,<2>,<3>,<4>` — its own
   values do not match what is actually strapped on this unit. Fixed in
   `dts/ipq5018-spnmx57.dts`; the diagnostic 32-address MDIO scan already
   covers address 0, so this fix is being tested empirically before deciding
   whether anything further is needed

## What decompiling the calibration loop's real caller revealed

Went to properly decompile the calibration loop (previously only hand-traced)
and its caller — `FUN_4a94c630` and `FUN_4a94d21c` in
`collected/uboot/appsbl-mtd6.bin`, via Ghidra. **This is a materially bigger
piece of code than the earlier hand-trace suggested.**

`FUN_4a94c630` (the calibration function) actually has THREE stages, not one:

1. A **clock-enable and reset-deassert sequence**, missed entirely by the
   earlier manual trace (which started reading mid-function). Two helper
   calls (`FUN_4a94c568`, `FUN_4a94c57e` — plausibly "enable clock" and
   "deassert reset") run across a small set of registers first, then a loop
   clearing bit 0 across a *range* of registers in steps of `0x20`, then more
   calls to the same two helpers on four more specific registers. This reads
   like real clock/reset sequencing for the SRDS/EPHY blocks, and was
   completely absent from the earlier analysis
2. A **gate condition** (`(value >> 16) - 1 < 2`, i.e. only proceeds if a
   read value is 1 or 2) before the per-port calibration loop runs at all —
   an unexplored precondition
3. The **per-port calibration loop itself**, confirmed close to the earlier
   hand-trace but with corrected exact bit constants (worth re-checking
   against the earlier hand-traced version before trusting either over the
   other — this decompiled version is authoritative)

`FUN_4a94d21c` (the function that calls the calibration routine, only once
`chip_ver_get`-equivalent detection matches id `0x17`) does **substantially
more** afterward: reads what look like MAC addresses via two more unknown
functions (`FUN_4a95a050`, `FUN_4a95a980`), runs a *second*, larger
reset-deassert loop (thirteen more register calls to `FUN_4a94d694`), branches
on what looks like a link-speed or status code, and finishes with port
enable/forwarding calls (`FUN_4a94cd04`, `FUN_4a94cd5c`, `FUN_4a94dca8`).

**None of `FUN_4a94c568`, `FUN_4a94c57e`, `FUN_4a94d694`, `FUN_4a94e004`,
`FUN_4a94cb98`, `FUN_4a94cfb8`, `FUN_4a94cdb4`, `FUN_4a94cd04`,
`FUN_4a94cd5c`, `FUN_4a94dca8`, `FUN_4a95a050`, `FUN_4a95a980` have been
decompiled or understood yet.**

## Honest assessment

This is not "one more small patch." It is a real driver bring-up sequence —
clock/reset management, calibration, and port/MAC/speed configuration — with
a dozen more functions still to resolve before it could be faithfully ported.
The earlier framing (in `docs/preinit-port.md` and the first cut of
`docs/uboot-qca8084-protocol.md`) undersold this. Whether all of it is
actually *required* just to get the EPHYs answering basic MDIO reads (as
opposed to being needed for full production-quality link-up) is unknown —
possible that a subset (maybe just the clock-enable/reset-deassert prefix)
is what actually unblocks detection, with the rest being port-config
polish. That is worth establishing empirically before porting the whole
tree blind.

## Recommended next steps, in order

1. See whether the address fix alone (0,1,2,3 instead of 1,2,3,4) makes any
   difference to the 32-address scan — cheap, already building, tells us
   whether *anything* responds at those addresses pre-calibration
2. If not: decompile `FUN_4a94c568`/`FUN_4a94c57e` next specifically (the
   clock-enable/reset-deassert helpers) — these are the most likely candidates
   for "the minimum needed to make the EPHYs respond at all," ahead of the
   full calibration loop and the much larger port/MAC/speed configuration in
   `FUN_4a94d21c`
3. Use the same Ghidra workspace (`~/ghidra-workspace` on this Mac, GUI at
   `http://localhost:6080/vnc.html` when the container is running) rather
   than re-setting-up tooling — it already has the whole binary analyzed
