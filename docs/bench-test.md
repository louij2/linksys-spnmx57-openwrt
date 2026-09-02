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
