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
