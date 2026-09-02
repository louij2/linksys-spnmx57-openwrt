# The one bench test that decides everything

There is a single read that splits the remaining unknowns. It needs the serial
console and nothing else. Do it before building anything.

## Why this read

The QCA8084's clocks come from `nss_cc_switch_core`, which can only synthesise
312.5 MHz from `UNIPHY1_TX312P5M` divided by one. If that parent PLL is not
already spinning, `clk_set_rate()` fails with `-EINVAL` and the driver cannot
recover on its own.

Whether the parent is already spinning depends on what U-Boot left behind. So:
read the chip and see whether it answers.

## Route A: from U-Boot, over serial. No build needed

Interrupt boot to get the U-Boot prompt, then:

```bash
mdio read 90000.mdio 1 2
```

Stock Qualcomm U-Boot may spell this `mii read` instead, and may want a bus
selected first. `help` at the prompt lists what this build actually has, and
`mii device` lists the buses. Read registers 2 and 3 (the PHY ID high and low
words) at addresses 1, 2, 3 and 4.

## Route B: from Linux on the flashed unit, over Wi-Fi

Install the tools once, then read:

```bash
opkg update && opkg install mdio-tools kmod-mdio-netlink
```

Then, for each address:

```bash
mdio 90000.mdio phy 1 2; mdio 90000.mdio phy 1 3
```

## How to read the answer

Registers 2 and 3 concatenate into the PHY ID.

| what comes back | what it means | what is needed |
|---|---|---|
| `004d:d180` | The parent PLL was already live, left running by U-Boot. The chip is awake and answering | The device tree work alone should be enough |
| `0xffff` or `0x0000` at every address | The parent is dead. The chip is unclocked, so the bus is floating | The driver reordering is needed as well as the device tree |

`0xffff` and `0x0000` mean the same thing here. `__qca8084_mii_read` only checks
`ret < 0`, so an unclocked bus returns one of those and the driver accepts it as
a real value. That is the silent failure.

For reference, the SoC's own internal GE PHY is on the **other** bus and is
known to answer, so it makes a good control that the tooling works:

```bash
mdio 88000.mdio phy 7 2; mdio 88000.mdio phy 7 3
```

That should give `004d:d0c0`. If it does not, the problem is the tool or the
syntax, not the hardware.

## The second thing to check while serial is attached

Whether U-Boot can boot a kernel from RAM. If it can, every device tree attempt
after this is a TFTP transfer and a reboot rather than a flash cycle:

```bash
help
printenv
```

Look for `tftpboot`, `bootm`, and what `ipaddr` / `serverip` are set to. If
`tftpboot` and `bootm` are both present, the iteration loop is safe and cheap
and nothing needs writing to flash until something actually works.

## What to record

Paste the whole serial session into `collected/`. The boot log from a stock
boot is worth having on its own, because it shows the vendor driver bringing
the package up in the correct order, which is the sequence we are trying to
reproduce.

---

# RESULT, run 2026-09-02

Run over SSH on the flashed unit (reached over IPv6 link-local on its own Wi-Fi,
because serial input does not work and its DHCP server is not running).

**Caveat that shapes everything below:** this unit runs the **SPNMX56** device
tree, so it never reaches the QCA8084 code path at all. These are reads of the
57's hardware through the 56's description.

## What answered, and what did not

| bus | address | result | how |
|---|---|---|---|
| `88000` | `0x07` | **`0x004dd0c0`** | live userspace read. Internal GE PHY |
| `90000` | `0x00`-`0x04` | **missing** | kernel MDIO probe at boot |
| `90000` | `0x11` | **device id `0x17`** | qca8k register read at boot |
| `90000` | `0x1c` | nothing real | see below |

## The switch is alive; the PHYs are not

```
[    1.847650] mdio_bus 90000.mdio-1: MDIO device at address 0 is missing.
                                    ... addresses 1, 2, 3, 4 likewise ...
[    1.888633] qca8k 90000.mdio-1:11: Switch id detected 17 but expected 13
```

`QCA8K_ID_QCA8337` is `0x13`, so those are hex. **A real register transaction
succeeded on the `90000` bus at address `0x11` and returned device id `0x17`.**

That is the important nuance, and it is new. Against Hyndland's table the EPHYs
reading as absent is the "dead parent" outcome, which implies the driver
reordering is needed and not just the device tree. But it is **not** a wholesale
dead bus: the MDIO master works, and the switch core is clocked enough to answer
before any `ethernet-phy-package`, `nsscc` node or strap fixup exists.

So the two halves are separable:

- **switch core and MDIO master**: already clocked, already reachable
- **the four EPHYs**: not clocked and not strapped, exactly what
  `phyaddr_fixup` / `mdio_clk_fixup` exist to do

## Nothing is at 0x1c, confirmed twice

`/sys/bus/mdio_bus/devices/90000.mdio-1:1c/phy_id` reads `0x004dd101`, but that
is the DT-asserted value: the 56's DTS hard codes
`compatible = "ethernet-phy-id004d.d101"`, so the PHY layer binds without
reading. The SSDK's independent read of the same address returned `0xfffafffa`.

## A trap in the SSDK userspace interface

`/sys/ssdk/phy_read_reg` accepts `0x<addr> 0x<reg>` and the **high byte selects
the MDIO bus**, which is how the boot message's `phy_adress = 284` decodes
(`0x11C` = bus 1, address `0x1c`).

Do not read verdicts out of it for bus 1. Its returns are status codes, not bus
data:

| read | meaning |
|---|---|
| bus 0, empty address | `0x0` |
| bus 1, **any** address | `0xfffa` |
| bus 2, 3, 7 (nonexistent) | `0xffff` |

A genuinely floating MDIO bus reads `0xffff`, not `0xfffa`, and `0xfffa` appears
at every bus 1 address identically. It is the SSDK saying it cannot route the
read, because the 56's DTS does not describe that bus to it. **The authoritative
evidence for bus `90000` is the kernel's own probe at boot, not this
interface.**

## Still outstanding

The clean version of this test still wants the candidate DT booted, so the
QCA8084 driver actually runs and the strap fixup is attempted. What we have
establishes the starting state, not the outcome.

Also still outstanding: **U-Boot access**, which needs serial input working. The
autoboot window is three seconds and interruptible, so the moment the adapter's
TX reaches the router's RX, booting test kernels from RAM becomes available.
