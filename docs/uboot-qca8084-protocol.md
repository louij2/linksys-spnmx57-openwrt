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

## RESOLVED — decompiled with Ghidra, no remaining ambiguity

The earlier draft of this page stopped here, honestly flagging a hand-traced
guess as unreliable. Ran Ghidra headless (Docker, `blacktop/ghidra`, local,
no cloud) against `appsbl-mtd6.bin` and decompiled the four functions
directly rather than continuing to hand-read Thumb bytes. The ELF loader
auto-detected `ARM:LE:32:v8` and placed everything at the correct addresses
with no manual base-address work needed.

**The earlier "Clause 45" guess was analyzing the wrong branch of `c35c`/
`c3d0` entirely** — that branch (gated on bit 30 of the `reg` argument) is
real code, used elsewhere in this binary, but our target functions never set
that bit, so it never executes for `EPHY_CFG`/`SERDES_CFG` access. Every
transaction our path actually takes is **plain Clause 22**.

Decompiled `0x4a94c4dc` (32-bit read) and `0x4a94c524` (32-bit write) in full
(reproduced here after variable renaming for clarity; the raw decompiler
output with `FUN_`/`DAT_` names is in `collected/uboot/`):

```c
uint32_t switch_reg_read32(uint32_t logical_addr)
{
    uint32_t page = (logical_addr & 0xffffff) >> 8;
    uint32_t slot = logical_addr & 0x1c;

    mdio_write(phy=0x18, reg=0x0c, val=page);   /* page select */
    udelay(100);
    uint16_t lo = mdio_read(phy=0x10, reg=slot);
    uint16_t hi = mdio_read(phy=0x10, reg=slot + 2);
    return lo | (hi << 16);
}

void switch_reg_write32(uint32_t logical_addr, uint32_t value)
{
    uint32_t page = (logical_addr & 0xffffff) >> 8;
    uint32_t slot = logical_addr & 0x1c;

    mdio_write(phy=0x18, reg=0x0c, val=page);   /* page select, same as read */
    udelay(100);
    mdio_write(phy=0x10, reg=slot,     val=value & 0xffff);
    mdio_write(phy=0x10, reg=slot + 2, val=value >> 16);
}
```

Where `mdio_write`/`mdio_read` are literally `bus->write`/`bus->read` —
**plain Clause 22 MDIO transactions**, the exact thing mainline's existing
`ipq4019_mdio_write_c22`/`read_c22` already implement correctly today. No new
low-level register-poke code is needed, and critically: **no `bus->priv`
struct-layout compatibility hack is needed for this piece at all.** This can
be built entirely on the standard `mdiobus_write()`/`mdiobus_read()` kernel
API against the unmodified driver.

Also decompiled the two low-level helpers these call, confirming the whole
chain end to end:

- `FUN_4a94c32c` (busy poll): loops up to 1000 times checking
  `*CMD_REG & 0x10000`, i.e. bit 16 — **exactly** mainline's
  `MDIO_CMD_ACCESS_BUSY = BIT(16)`. Functionally identical to
  `ipq4019_mdio_wait_busy()`, just iteration-counted instead of
  timeout-based.
- `FUN_4a960708` (the `100`-argument delay call): a standard chunked
  microsecond delay loop. Confirms the `udelay(100)` after page-select above.

**Sanity check against the two known addresses**, confirming the page/slot
split makes sense:

| register | logical address | page (bits 23:8) | slot (bits 4:2, ×4) |
|---|---|---|---|
| `EPHY_CFG` | `0x0C90F018` | `0x90F0` | `0x18` |
| `SERDES_CFG` | `0x0C90F014` | `0x90F0` | `0x14` |

Same page, different 4-byte slot within it — exactly what "one 256-byte page,
individually addressed 32-bit registers every 4 bytes" predicts, and matches
the two registers being adjacent-but-distinct fields the vendor source treats
as siblings.

**The calibration loop's PHY debug-register writes (`0x1D`/`0x1E` at each
EPHY's own address) are separately confirmed the same way** — they call the
identical `c35c` primitive but with the EPHY's real address (1/2/3/4, not the
fixed `0x18`/`0x10` pseudo-addresses) and a register number without bit 30
set, so they take the same plain-C22 path. Nothing special needed there
either.

## What NOT to do

Do not transcribe the `0x4a94c4dc`/`0x4a94c524` bit-packing from the notes in
an earlier draft of this page (since corrected) or from memory of this
analysis session without re-verifying against the actual disassembly or,
better, a Ghidra-confirmed trace. A subtly wrong register write into
`0x90044`/`0x90048`/`0x90050` is not caught by a compiler — it would show up
only as another confusing boot log, or worse, silently wrong behaviour.
