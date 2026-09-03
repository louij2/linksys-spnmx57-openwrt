# The vendor U-Boot's QCA8084 access protocol, reverse-engineered

Extracted 2026-09-03 from `mtd6` (`0:APPSBL`) on the live device, once the
serial console started working both directions. This is the exact mechanism
the vendor's own U-Boot uses to talk to the QCA8386/QCA8084 — the thing we
have been trying to reverse-engineer since `docs/preinit-port.md`.

## How

```
ssh root@<router> 'cat /dev/mtd6' > appsbl.bin   # 1,310,720 bytes, mtd6 = "0:APPSBL"
```

It is a raw ELF32 ARM binary (`file` confirms `ELF 32-bit LSB executable, ARM,
EABI5`), one big LOAD segment: file offset `0x12000` → vaddr `0x4a920000`,
size `0x6b6f0`. Apple's own LLVM `objdump` (Xcode CLT, no extra install)
parses and disassembles it directly — the code is Thumb-2, not ARM32, despite
`objdump -f` reporting plain `arm` (ARM32 decode of Thumb bytes produces
convincing-looking garbage; re-running with `--triple=thumbv7-none-eabi`
fixed it immediately). No `readelf`, no `capstone` needed.

Committed: `collected/uboot/appsbl-mtd6.bin`,
`collected/uboot/disasm-switch-detect-and-calib.txt`.

## The register map

Confirmed by tracing the low-level read/write primitives (functions at
`0x4a94c35c` write, `0x4a94c3d0` read) back to their literal-pool constants:

| register | address |
|---|---|
| `MODE_REG` | `0x90040` |
| `ADDR_REG` | `0x90044` |
| `DATA_WRITE_REG` | `0x90048` |
| `DATA_READ_REG` | `0x9004c` |
| `CMD_REG` | `0x90050` |

**These are the exact same MDIO controller mainline `mdio-ipq4019.c` already
drives** (`0x90000` = `mdio@90000` in the DTS) — same base, and the offsets
line up with mainline's own `MDIO_MODE_REG`/`MDIO_ADDR_REG`/
`MDIO_DATA_WRITE_REG`/`MDIO_DATA_READ_REG`/`MDIO_CMD_REG`. **This is not a
different bus or a custom peripheral. It is the same controller, used with an
extra mode bit and a two-phase command sequence** that mainline's driver never
sets, because it only ever does plain Clause 22 single-register access.

## The protocol

**Mode values**, written to `MODE_REG`:

| value | meaning |
|---|---|
| `0x1500f` | plain Clause 22 (what mainline effectively already does, modulo the C45 bit mainline explicitly clears) |
| `0x1510f` | 32-bit indirect switch-register access — exactly one extra bit (`0x100`) over the C22 value |

**Command sequences**, written to `CMD_REG`, always as two phases with a busy
poll after each:

| operation | phase 1 | phase 2 |
|---|---|---|
| 32-bit write | `0x100` | `0x101` |
| 32-bit read | `0x100` | `0x102` |

**Fixed pseudo-PHY address: `0x18`.** Every switch-internal register access
(read or write) addresses this fixed value on the MDIO bus, packed into
`ADDR_REG` as `(0x18 << 8) | reg_field`, where `reg_field` is derived from the
logical 32-bit register address (see below). `0x18` never changes; it is not
one of the four EPHY addresses, and not the `0x11` address `qca8k` uses to
read the switch ID under the SPNMX56 DTS. It is a separate, fixed
"indirect-access" pseudo-address that the chip itself reserves for this
purpose — consistent with the standard Qualcomm/Atheros switch family
convention of a special page-select PHY address distinct from the real ports.

**Address field derivation**, transcribed from `0x4a94c4dc` (the higher-level
`read(logical_addr)` wrapper): given a logical address like
`EPHY_CFG = 0x0C90F018`,

```
page_hi  = (logical_addr >> 24) & 0x1f      // 0x0C → 0x0c
reg_lo   = (page_hi >> 5)  | 0x18           // folds into the phy_addr, effectively 0x18 here
reg_mid  = (logical_addr >> 16) & 0x1f      // bits [20:16] of the address
```

then `reg_mid` (page-like value) and the low word of the address together form
the value written to `ADDR_REG`. The exact bit-packing is preserved verbatim
in the disassembly excerpt below rather than re-derived from first principles
— transcribe the instruction sequence directly into the kernel patch rather
than trusting a hand re-derivation of the bit arithmetic, it is easy to get an
off-by-one-bit wrong by hand.

## The two known logical addresses

| name | value |
|---|---|
| `EPHY_CFG` | `0x0C90F018` |
| `SERDES_CFG` | `0x0C90F014` |

Both match the values already known from `qca-ssdk`'s `mht_reg.h`
(`EPHY_CFG_OFFSET`, `SERDES_CFG_OFFSET`), confirming this is genuinely the
same register, just reached by a different (working) transport.

## What U-Boot actually does with it — and a finding that changes the plan

Read `EPHY_CFG` in a loop for `port = 0..3`, extracting the CURRENT 5-bit PHY
address field for each port (`(value >> (port*5)) & 0x1f`). **It never writes
new address values into those fields.** It only reads whatever is already
there.

For each port, it also reads a *different*, per-port register
(`0xC900048`/`0xC900060`/`0xC900068`/`0xC90005C` — four distinct addresses,
not a simple linear table) and extracts two bitfields from it, then writes
those as calibration/trim values into the PHY's own MDIO **debug registers
0x1D / 0x1E** — the standard Qualcomm/Atheros PHY extended-register access
mechanism (the same one `at803x.c` uses in mainline Linux). This is per-port
analog calibration, not address assignment.

After the loop, it re-reads `EPHY_CFG` once more and clears bits `[21:20]`
(`& ~0x300000`), then writes it back — the last step before returning.

**So the strap addresses are not programmed by software in this code path at
all.** Either they are a hardware default (pull-up/pull-down strapping on the
PCB, or a fixed silicon default) already sitting at 1/2/3/4 before any driver
runs, or they are set somewhere earlier in the boot chain (PBL/SBL1) that this
disassembly does not cover. **This has not been confirmed by reading
`EPHY_CFG`'s value directly — that is the next concrete test**, and it changes
the shape of the Linux port: if the addresses are already correct in hardware,
the missing piece for Linux may be *only* the calibration loop and the final
enable-bit clear, not an address-programming step at all.

## What this means for the `mdio-ipq4019.c` patch

Smaller than originally scoped in `docs/preinit-port.md`. Needed:

1. `sw_read32(base, addr)` / `sw_write32(base, addr, val)` — the two-phase
   `MODE_REG`/`ADDR_REG`/`DATA_*_REG`/`CMD_REG` sequence above, transcribed
   from the disassembly rather than re-derived
2. The calibration loop (4 ports, read a per-port register, write two
   bitfields to MDIO debug regs 0x1D/0x1E at whatever address `EPHY_CFG`
   currently reports for that port)
3. The final `EPHY_CFG &= ~0x300000` read-modify-write
4. Gated on new DT properties so nothing changes for boards that do not ask
   for it — matching the vendor's `phyaddr_fixup`/`uniphyaddr_fixup`/
   `mdio_clk_fixup` naming makes sense, even though what actually runs is
   calibration rather than address programming

Still true from `docs/preinit-port.md` and worth re-reading before writing
the patch: the `bus->priv` struct layout mismatch between what qca-ssdk
expects and what `ipq4019_mdio_data` provides is a real memory-safety bug,
independent of any of the above.
