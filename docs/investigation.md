# Investigation notes

## The error

```
Qualcomm QCA8084 90000.mdio-1:00: probe failed with error -22
```

`-22` is `-EINVAL`. The QCA8084 PHY driver was matched and then rejected what it
was given, so the node exists but is described wrongly. Candidates, in the order
worth checking:

1. **Wrong PHY address.** The node says `mdio-1:00` (address 0) while the thread
   reports the PHY at `0x1c`. If the real device answers at 0x1c and the DTS
   declares 0, probe gets a device that is not what it expects
2. **Missing required properties.** QCA8084 needs clocks and resets described;
   an inherited SPNMX56 node may not carry them
3. **MDIO bus structure.** Two buses exist (`mdio-0`, `mdio-1`). The switch and
   the 2.5G PHY may not be on the bus the DTS claims
4. **GMAC mode / port mapping.** Even once the PHY probes, the MAC-to-PHY link
   needs the right `phy-mode` and port layout

## Ground truth to gather first

Do not guess from the SPNMX56 DTS. From the running device:

- `/sys/firmware/fdt` — exactly what the kernel booted with, decompile and read
- `/sys/bus/mdio_bus/devices/` — which PHYs actually enumerated, and at what
  addresses. This is the single most informative thing available
- full `dmesg` — the ordering of mdio, switch and PHY messages says which stage
  failed

## Comparison targets

- SPNMX56 DTS in OpenWrt, which this was derived from
- Other IPQ5018 boards using QCA8084, for a node that is known to probe

## Safety

A bad DTS means a device that does not boot. **Establish recovery before
flashing**: second firmware partition, TFTP recovery, or UART. Wi-Fi working is
what makes iteration cheap, but it does not help if the kernel never starts.

## The delta is bigger than a tweak (found while scaffolding)

The SPNMX56 base DTS (`dts/ipq5018-spnmx56.dts`, 197 lines) describes:

```
qca8081: ethernet-phy@28 {           // 28 decimal = 0x1c
    compatible = "ethernet-phy-id004d.d101";
};
switch1: ethernet-switch@17 {
    compatible = "qca,qca8337";      // 5-port switch, ports 0..4
};
switch_mac_mode = <MAC_MODE_SGMII_CHANNEL0>;
```

So the **56** is: a QCA8337 five-port switch, plus a single QCA8081 2.5G PHY at
0x1c, on two MDIO buses.

The **57**'s failure message is:

```
Qualcomm QCA8084 90000.mdio-1:00: probe failed with error -22
```

Note two differences, not one:

1. It is a **QCA8084**, not a QCA8081. The 8084 is a quad PHY that integrates
   what the 8337 + 8081 did separately on the 56
2. It is declared at **address 00**, where the 56's 2.5G PHY sits at **0x1c**

That means the 57 is not "the 56 with a tweak": the Ethernet topology itself
differs. Whoever wrote the 57 DTS appears to have swapped the compatible string
without re-describing the bus, which fits `-EINVAL` exactly.

### So the first question is a hardware one

**What is actually on the 57's board, and at which MDIO addresses?** Three ways
to find out, in increasing order of effort:

1. `scripts/collect.sh` on the running unit — `/sys/bus/mdio_bus/devices/` lists
   what enumerated, which is ground truth
2. **Look at the board.** Chip markings settle the 8084-versus-8337+8081
   question in seconds, and UART fitting means it will be open anyway
3. **Vendor GPL source.** Linksys published a GPL dump for the 56
   (`domenpk/Linksys_SPNMX56TB_v1.0.1.216589`). If an equivalent exists for the
   57, its device tree answers every one of these questions directly and is by
   far the biggest shortcut available

### And a free source of truth worth remembering

You have **more than one of these routers, and only one is flashed.** A unit
still on stock firmware carries the vendor's own DTB, which describes this exact
board correctly by definition. Dumping it beats inferring anything from the 56.
