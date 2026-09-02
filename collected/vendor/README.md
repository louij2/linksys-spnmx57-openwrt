# Vendor device tree, extracted from stock firmware

Obtained with no hardware access at all: no case opened, nothing reflashed.

## Provenance

Stock OEM image, linked from the OpenWrt thread:

    http://download.linksys.com/updates/20250403t130837/FW_MX57CF_1.0.1.216553_prod.img

    size  54263808 bytes
    md5   62b76e25b194ecd42275460a7eedcace   (verified on download)

The `.img` is a u-boot FIT image, which is itself a flattened device tree, so
`dtc` reads it directly. `fit-structure.dts` is its structure with the payload
data elided. It contains two subimages:

| subimage | description | notes |
|---|---|---|
| `kernel@1` | ARM OpenWrt Linux-5.4.213 | gzip, load/entry `0x41208000` |
| `fdt@1` | ARM OpenWrt **Palm15** device tree blob | uncompressed, 43034 bytes |

`Palm15` is the vendor board codename for this model.

## How it was extracted

The FIT stores `fdt@1` uncompressed, so the inner blob is findable by its
`d00dfeed` magic and copied out by its own `totalsize`:

    offset 0x00000000  totalsize 4868476   <- the FIT itself
    offset 0x00499c30  totalsize   43034   <- the vendor DTB

`vendor-palm15.dtb` is that blob byte for byte. `vendor-palm15.dts` is
`dtc -I dtb -O dts` of it.

## Reproducing

    curl -fLO http://download.linksys.com/updates/20250403t130837/FW_MX57CF_1.0.1.216553_prod.img
    md5 FW_MX57CF_1.0.1.216553_prod.img   # 62b76e25b194ecd42275460a7eedcace
    dtc -I dtb -O dts -o vendor-palm15.dts vendor-palm15.dtb

The image is deliberately not committed here; it is 52 MB and is a stable
public download.

## Caveat worth keeping in mind

The vendor tree self identifies as:

    model = "Qualcomm Technologies, Inc. IPQ5018/AP-MP03.1";

which is the Qualcomm reference board. Linksys adapted the reference DTS rather
than writing a fresh one, so some nodes in it are reference board boilerplate
that this board does not populate. The Ethernet nodes are clearly board
specific and were changed; treat unrelated nodes with more suspicion.
