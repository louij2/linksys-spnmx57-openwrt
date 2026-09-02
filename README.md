# linksys-spnmx57-openwrt

Getting Ethernet working on OpenWrt on the **Linksys SPNMX57** (Community Fibre
supplied, Qualcomm **IPQ5018**).

## Status

OpenWrt boots and most of the device works. **Ethernet does not.**

| | |
|---|---|
| Kernel boot | works |
| PCIe | works |
| Wi-Fi (QCN9074, ath11k) | works |
| UBI + overlay | works |
| **Ethernet** | **fails** |

The failure, from the boot log in the [upstream
thread](https://forum.openwrt.org/t/openwrt-support-for-linksys-spnmx57-variants/231653):

```
Qualcomm QCA8084 90000.mdio-1:00: probe failed with error -22
```

`-22` is `-EINVAL`: the driver rejected its device tree node. That is a
description problem, not a missing driver.

## Why this is tractable

Unlike a from-scratch SoC port, everything needed already exists upstream:

- **IPQ5018 is a supported OpenWrt target.** No SoC bring-up required
- The drivers exist: `qca8k` for the switch, the QCA8084 PHY driver, `ath11k`
- **Wi-Fi works**, so the device is reachable for testing even with Ethernet dead
- **UART is available on this unit**, so a failed boot is diagnosable rather than
  fatal, and a test kernel can potentially be booted from RAM without flashing
- Someone in the thread with UART access has offered to test patches

The thread's conclusion is that the SPNMX57's DTS was derived from the SPNMX56
and the hardware differs: switch definition, PHY node address, MDIO bus
structure, GMAC modes and port mapping.

## Known hardware

| | |
|---|---|
| SoC | Qualcomm IPQ5018 |
| Switch | QCA8327 / QCA8337 (switch ID 17) |
| PHY | QCA8081 / QCA8084 at `mdio-1:0x1c` |
| Wi-Fi | QCN9074 over PCIe (ath11k) |

**Treat all of that as claimed, not confirmed.** Confirming it on the actual unit
is step one, because a DTS written from another model's assumptions is exactly
what is suspected to be wrong.

## Approach

1. **Get ground truth from the running device.** `scripts/collect.sh` dumps the
   live device tree the kernel actually booted with (`/sys/firmware/fdt`), the
   MDIO bus contents, the full boot log and the partition layout. Reading the
   real MDIO bus tells us which PHYs exist at which addresses, rather than
   inheriting a guess
2. **Decompile and diff** the running DTB against the SPNMX56 source, to see
   exactly what was inherited
3. **Fix the DTS**: switch node, PHY address, MDIO structure, GMAC modes, port
   mapping
4. **Test, iteratively.** Wi-Fi staying up is what makes this safe
5. **Upstream it** to the thread once Ethernet works

## Recovery

**UART headers are being fitted to this unit.** That is what makes the whole
project practical: a bad DTS becomes a boot log to read rather than a brick, and
U-Boot gives a route back in. Iterating on device tree without serial means every
failed attempt is a recovery exercise.

Still worth doing before the first flash:

- [ ] Capture a **known-good boot log** over UART, for comparison
- [ ] Confirm what U-Boot offers: TFTP recovery, a second firmware partition,
      `bootm` from RAM. Booting a test kernel from RAM without flashing is by far
      the safest iteration loop if it is available
- [ ] Keep the stock image and note exactly how to get back to it
