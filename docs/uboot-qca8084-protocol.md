# The vendor U-Boot's QCA8084 access, reverse-engineered — solid parts and open ones

Extracted 2026-09-03 from `mtd6` (`0:APPSBL`) on the live device, once the
serial console started working both directions. This is an attempt to
reverse-engineer the exact mechanism the vendor's U-Boot uses to talk to the
QCA8386/QCA8084 — the thing `docs/preinit-port.md` scoped as the remaining
blocker.

**Read this whole page before writing a kernel patch from it.** Part of it is
confirmed with certainty. Part of it is a best-effort trace that hand-decoding
raw Thumb-2 bytes without cross-reference tooling could easily have gotten
wrong in a way that looks plausible. They are marked separately below.
Mistaking the second kind for the first is exactly how a confidently wrong
patch gets written.

## How this was done

```
ssh root@<router> 'cat /dev/mtd6' > appsbl.bin   # 1,310,720 bytes, mtd6 = "0:APPSBL"
```

Raw ELF32 ARM (`file`: `ELF 32-bit LSB executable, ARM, EABI5`), one LOAD
segment: file offset `0x12000` → vaddr `0x4a920000`, size `0x6b6f0`. Apple's
own LLVM `objdump` (Xcode CLT, nothing extra installed) parses it. The code is
Thumb-2, not ARM32, despite `objdump -f` reporting plain `arm` — decoding ARM32
bytes that are actually Thumb halfwords produces convincing-looking garbage;
`--triple=thumbv7-none-eabi` fixed it immediately, which is the first thing to
check if a disassembly looks like nonsense with real-looking opcodes.

Committed: `collected/uboot/appsbl-mtd6.bin`,
`collected/uboot/disasm-full-appsbl.txt` (full ~188k line disassembly),
`collected/uboot/disasm-switch-detect-and-calib.txt` (the relevant excerpt),
`collected/uboot/mainline-mdio-ipq4019.c-for-reference.c` (mainline's current
driver, for comparing register offsets byte for byte).

## SOLID: the register offsets are byte-identical to mainline's own driver

Traced the low-level MDIO transaction primitives back to their literal-pool
constants:

| register | address |
|---|---|
| `MODE_REG` | `0x90040` |
| `ADDR_REG` | `0x90044` |
| `DATA_WRITE_REG` | `0x90048` |
| `DATA_READ_REG` | `0x9004c` |
| `CMD_REG` | `0x90050` |

Compare against `mdio-ipq4019.c` (`collected/uboot/mainline-mdio-ipq4019.c-for-reference.c`,
lines 16-34): `MDIO_MODE_REG=0x40`, `MDIO_ADDR_REG=0x44`,
`MDIO_DATA_WRITE_REG=0x48`, `MDIO_DATA_READ_REG=0x4c`, `MDIO_CMD_REG=0x50`,
all relative to the same `0x90000` base as `mdio@90000` in the DTS. **Exact
match, to the byte.** This is not a different bus or peripheral — it is the
identical controller mainline already drives correctly for plain PHY access
(confirmed working: our internal GE PHY reads fine at address 7 on
`mdio@88000`, and this SoC family shares the same controller design across
both MDIO instances).

## SOLID: the calibration loop uses plain Clause 22

`chip_ver_get`'s companion calibration routine (starts ~`0x4a94c6b6`, the
excerpt in `disasm-switch-detect-and-calib.txt`) loops over 4 ports, reads a
per-port register, and writes two extracted bitfields into that port's own
PHY via MDIO registers **`0x1D`/`0x1E`** — the standard Qualcomm/Atheros
"debug register" access convention (the same one mainline `at803x.c` already
uses for other Qualcomm PHYs). Confirmed by checking the call sites: these go
through the SAME branch as the "simple" 2-register path (`beq` taken, bit 30
of the caller's `reg` argument clear), which mainline's existing
`ipq4019_mdio_write_c22`/`read_c22` already implement correctly. **This part
needs no new low-level protocol code** — just a small port-loop calling the
existing C22 read/write with the right register numbers, once the phy address
for each port is known.

## SOLID: it does not program new EPHY addresses

The loop reads the CURRENT 5-bit address field per port out of `EPHY_CFG`
(`0x0C90F018`) and uses it as-is; it never writes new values into those
fields. After the loop it re-reads `EPHY_CFG` once more, clears bits `[21:20]`,
and writes it back. **This has not been independently confirmed against the
live chip** — whether the 4 fields already hold `1,2,3,4` before any of this
runs is unknown; it could be a hardware/silicon default, or set earlier in
the boot chain (PBL/SBL1) than this binary covers.

## UNCERTAIN: exactly how `EPHY_CFG`/`SERDES_CFG` themselves are read and written

This is the part to be honest about. Initial trace suggested the 32-bit
switch-internal register access (as opposed to the plain-C22 calibration
writes above) maps cleanly onto mainline's Clause 45 support
(`read_c45`/`write_c45`, which already exist and already work) — the
command-code sequence (`0x100` then `0x101` for write, `0x100` then `0x102`
for read) matches mainline's `MDIO_CMD_ACCESS_CODE_C45_ADDR/_WRITE/_READ`
exactly.

**But re-tracing the actual caller (the `chip_ver_get`-adjacent read wrapper
at `0x4a94c4dc`) shows something more elaborate**: it makes a first call into
what looked like the write primitive (`0x4a94c35c`) with an argument that does
not look like a normal Clause 45 `(mmd, reg)` pair, followed by two separate
calls into the read primitive (`0x4a94c3d0`) at addresses that differ by `2`,
combined as `low16 | (high16 << 16)`. That shape (address-latch write, then
two 16-bit reads at a `+2` stride) is a real, known pattern for 32-bit access
over a 16-bit MDIO register space — but which of the several plausible
concrete bit-packings applies is genuinely not nailed down here, and I caught
one internal mis-attribution partway through this trace (assigning a
"phy-address-like" role to a register value that turned out on closer reading
to plausibly be something else). That is exactly the kind of error that is
easy to make and hard to notice when hand-reading Thumb bytes serially without
a proper cross-referencing disassembler, and I would rather flag it than
present a guess as fact.

## Recommended next step, before writing the kernel patch

**Get a real decompiler onto the analysis, not further hand-tracing.**
Ghidra (free, scriptable, handles Thumb-2/ARM interworking and builds proper
call graphs and xrefs automatically) would resolve the remaining ambiguity in
one clean pass rather than the error-prone manual approach used here. Load
`collected/uboot/appsbl-mtd6.bin` as raw ARM, base address `0x4a920000` minus
`0x12000` (i.e. image base `0x4a90e000`, since Ghidra wants the base for
offset 0, not the LOAD segment's vaddr directly — check this against the ELF
program header when loading), and let it auto-analyze. The two functions to
retarget are at vaddr `0x4a94c4dc` (32-bit register read) and `0x4a94c524`
(32-bit register write); a proper disassembler will show their true parameter
types and the exact bit-packing without the risk of manual mis-tracing.

Alternatively: the calibration-loop part alone (solid, above) could be tried
as a standalone experiment using only mainline's existing, already-correct
C22 read/write — **if** the hypothesis that EPHY_CFG already holds `1,2,3,4`
by hardware default turns out to be true, reading it is not even needed to
try the calibration writes. That has not been tested and is a cheap, low-risk
next step in its own right, independent of resolving the `EPHY_CFG` protocol
question.

## What NOT to do

Do not transcribe the `0x4a94c4dc`/`0x4a94c524` bit-packing from the notes in
an earlier draft of this page (since corrected) or from memory of this
analysis session without re-verifying against the actual disassembly or,
better, a Ghidra-confirmed trace. A subtly wrong register write into
`0x90044`/`0x90048`/`0x90050` is not caught by a compiler — it would show up
only as another confusing boot log, or worse, silently wrong behaviour.
