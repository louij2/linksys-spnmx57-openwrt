# What the serial console actually showed

Captured 2026-09-02 over UART at 115200 from the flashed unit, full boot from
power on. Raw log: `collected/uart-boot-2026-09-02.log` (426 lines).

This is the first real hardware evidence in this project, and it changes
several things.

## 1. The flashed unit is running the SPNMX56 device tree

```
[    0.000000] Machine model: Linksys SPNMX56
     Description:  ARM64 OpenWrt linksys_spnmx56 device tree blob
     Using 'config@mp03.1' configuration
```

Not a 57 DTS with a bad QCA8084 node. **The 56's DTS, unmodified.** So the
`-22` quoted in the thread came from somebody else's build, not from this box.
Everything this unit reports is the 56's description meeting the 57's hardware.

Build: OpenWrt `r31810-05feabfd09`, Linux **6.12.57**, with `qca-ssdk` and
`nss-dp` loaded, so it is the qualcommax NSS stack rather than mainline.

## 2. The QCA8386 is alive on MDIO, right now, with no fixups

The single most important line in the log:

```
[    1.888633] qca8k 90000.mdio-1:11: Switch id detected 17 but expected 13
```

`QCA8K_ID_QCA8337` is `0x13`, so these are hex. A switch at MDIO address
**0x11** answered a real register read and returned device id **0x17**, which
is not a QCA8337. Given the vendor tree puts a QCA8386 on exactly this bus,
`0x17` is the QCA8386.

**This matters for Hyndland's question.** It means the MDIO master and the
switch's own MDIO slave are clocked well enough to answer *before* any
`ethernet-phy-package`, `nsscc` clock controller or strap fixup exists. The
clock parent liveness problem is therefore not total: something in that chip is
already running when Linux gets there.

It does not prove the EPHY side is clocked. See the next point.

## 3. The four EPHYs are not there yet, exactly as predicted

```
[    1.847650] mdio_bus 90000.mdio-1: MDIO device at address 0 is missing.
[    1.847848] mdio_bus 90000.mdio-1: MDIO device at address 1 is missing.
[    1.853207] mdio_bus 90000.mdio-1: MDIO device at address 2 is missing.
[    1.859840] mdio_bus 90000.mdio-1: MDIO device at address 3 is missing.
[    1.866436] mdio_bus 90000.mdio-1: MDIO device at address 4 is missing.
```

The 56's DTS declares QCA8337 PHYs at 0 to 4 and none answer. That is what the
vendor's `phyaddr_fixup` / `mdio_clk_fixup` machinery exists to solve: the
EPHYs do not respond until they are clocked and strapped to 1..4.

So the picture is split: **switch reachable, PHYs not**. That is a more precise
statement than "the whole bus is dead", which is what the earlier userspace
scan in the thread suggested.

## 4. There is definitively nothing at 0x1c

```
[    6.415671] Qualcomm QCA8081 90000.mdio-1:1c: attached PHY driver
[    5.924794] ssdk_phy_driver_init[341]:INFO:dev_id = 0, phy_adress = 284,
               phy_id = 0xfffafffa phytype doesn't match
```

The attach is a phantom. The 56's DTS hard codes
`compatible = "ethernet-phy-id004d.d101"`, which makes the PHY layer bind
without reading the ID. When the SSDK actually read the device it got
`0xfffafffa`, which is a floating bus.

**Confirms the vendor tree.** The 57 has no QCA8081 and nothing at `0x1c`.

## 5. Both Wi-Fi radios work

Answers the open question in `roadmap.md` 5a.

```
[   15.192465] ath11k c000000.wifi: ipq5018 hw1.0        <- internal 2.4 GHz
[   17.946474] ath11k_pci 0000:01:00.0: qcn9074 hw1.0    <- 5 GHz on PCIe
```

Both reach `br-lan` as `phy0-ap0` and `phy1-ap0`. Two radios, both up.

Note the firmware build dates differ a lot: the internal radio runs a 2022-08-04
build, the QCN9074 a 2024-09-23 one.

## 6. Flash is SPI NAND, and the vendor tree was misleading

From U-Boot:

```
NAND:  QPIC controller support serial NAND
Serial Nand Device Found With ID : 0xc8 0x85
Serial NAND device Manufacturer:GD5F4GM8REYIG
Device Size:512 MiB, Page size:2048, Spare Size:128, ECC:8-bit
```

GigaDevice **GD5F4GM8REYIG**, 512 MiB serial NAND.

So `ipq5018-mx-base.dtsi`'s `compatible = "spi-nand"` is correct, and the
vendor tree's parallel `qcom,nandcs` node was AP-MP03.1 reference board
boilerplate after all. Answers `roadmap.md` 5e.

## 7. It is booting from the alt partition

```
Kernel command line: init=/sbin/init rootfstype=squashfs ubi.mtd=alt_rootfs
                     root=mtd:squashfs rootwait root=/dev/ubiblock0_0
NAND read: device 0 offset 0x58c0000, size 0x800000
```

`ubi.mtd=alt_rootfs`. OpenWrt went into the **alternate** firmware slot, which
means the stock image may still be intact in the primary slot. Worth confirming
from `/proc/mtd`, because it is a free route back to vendor firmware and
potentially to a stock boot log showing the correct QCA8084 bring up order.

## 8. U-Boot details, and recovery

```
U-Boot 2016.01 (Jul 26 2024 - 11:30:00 +0000)
CBT U-Boot ver: 7.3.21  ([IPQ5018].[SPF12.1].[CS1])
Hit any key to stop autoboot:  3  2  1  0
machid: 8040001
```

Autoboot is interruptible with a three second window, so a U-Boot prompt is
available once console input works. Also worth noting:

```
eth0 MAC Address from ART is not valid
eth1 MAC Address from ART is not valid
PCI1 is not defined in the device tree
```

The MAC addresses are not in ART, consistent with the base dtsi reading them
from the `devinfo` partition. And only one PCIe controller is in use, matching
the vendor tree disabling `pci@80000000`.

## 9. The unit is a Wi-Fi client on the home LAN

```
[   33.253252] phy1-sta0: authenticate with AA:BB:CC:DD:EE:FF
[   33.285298] phy1-sta0: associated
```

It joins an existing AP over its 5 GHz radio. It does not answer SSH from that
side, which is expected: the uplink lands in the `wan` firewall zone. So serial
remains the way in.

---

## Current console limitation

Reading works. **Writing does not.** The build does offer a console login:

```
[   13.081323] procd: - init -
Please press Enter to activate this console.
```

but Enter produces no response, so the adapter's TX is not reaching the
router's RX. Until that is fixed this is a read only console, which is enough
for boot logs but not for the MDIO bench test or for U-Boot.

## What this changes

| Before | After |
|---|---|
| Whole `90000` MDIO bus assumed dead | Switch answers at `0x11`; only the EPHYs are missing |
| `-22` assumed to be this unit's failure | This unit runs the 56 DTS and never gets that far |
| Unknown whether 2.4 GHz radio works | Both radios confirmed working |
| Flash description uncertain | Confirmed SPI NAND, GD5F4GM8REYIG |
| No known recovery path | Booting from `alt_rootfs`; primary slot may hold stock |
