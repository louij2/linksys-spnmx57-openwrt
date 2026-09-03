# The MDIO bus map, and what it settles

Taken 2026-09-03 on the running SPNMX57 with MHT compiled in and a 32-address
diagnostic scan in the DTS. Raw data: `collected/mdio-scan-90000.txt`,
`collected/uart-spnmx57-mht-boot.log`.

## MHT is now genuinely compiled in

```
strings /lib/modules/6.12.57/qca-ssdk.ko | grep -ci mht   ->  303   (was 0)
```

and a line appears that never has before:

```
[   14.681984] ssdk_dt_parse[1446]:INFO:switch node is qca8386!
```

So `MHT_ENABLE=enable` works, our `ess-switch1@1` node is recognised, and the
SSDK now knows it is looking at a QCA8386. That is three of the four things
needed.

Getting there required a patch to qca-ssdk itself: `qca808x.c`'s
`match_phy_device` still uses the pre-6.12 one-argument signature, so
`IN_QCA808X_PHY=TRUE` (which MHT pulls in) fails to build on kernel 6.12. See
`package/kernel/qca-ssdk/patches/100-qca808x-match_phy_device-6.12-signature.patch`.
**That is an upstream bug in `openwrt/qca-ssdk`, not something specific to this
board.**

## But detection still fails

```
[   14.689195] regi_init[2578]:INFO:qca-ssdk module init, no device found!
```

## The bus map

`mdio@88000` (control, known good):

| addr | phy_id |
|---|---|
| `0x07` | **`0x004dd0c0`** — IPQ5018 internal GE PHY, valid |

`mdio@90000`, all 32 addresses probed by the kernel before qca-ssdk loads:

| addr | result |
|---|---|
| `0x00`-`0x03` | missing (`0xffff`) |
| `0x04`, `0x05` | answered, `phy_id = 0x00000000` |
| `0x06`-`0x0f` | missing (`0xffff`) |
| `0x10`-`0x13` | answered, `phy_id = 0x00000000` |
| `0x14`-`0x1f` | answered, `phy_id = 0xb00eb00e` |

**No valid PHY ID anywhere.** QCA8084 would be `0x004dd180`. `0x00000000` and a
`0xb00eb00e` that repeats identically in registers 2 and 3 are both artefacts of
a bus with nothing properly driving it, not real devices. The structured pattern
(some addresses `0xffff`, others `0x0000`, others a repeating word) suggests the
chip is present and partially responsive but not initialised, rather than absent.

## What this settles

The four EPHYs are **not** reachable at 1-4, or at any other address, on a cold
chip. That is exactly the prediction: their addresses come from the `EPHY_CFG`
strap at `0xC90F018`, which qca-ssdk only ever **reads**
(`qca_mht_ephy_addr_get`) and never writes.

So the remaining blocker is confirmed by direct measurement rather than by
reading code: **nothing in OpenWrt straps or clocks the QCA8084 package.** QSDK
does it in the MDIO driver (`mdio-qca.c` binds `qcom,qca-mdio` and calls
`qca_mht_preinit()` at probe, driven by `phyaddr_fixup` / `uniphyaddr_fixup` /
`mdio_clk_fixup`). OpenWrt binds mainline `mdio-ipq4019.c`, which has none of it.

## Remaining work, in order

1. **Port `qca_mht_preinit()` into `mdio-ipq4019.c`**, gated on the `*_fixup`
   DT properties: program the EPHY and PCS MDIO addresses, then run the clock
   init (SRDS0/1 enable+reset, EPHY0-3 SYS clocks, deassert EPHY DSP in
   `GCC_GEPHY_MISC`, efuse load, analog fixups, settle). This is the main piece
2. Re-run this scan. Success looks like `0x004dd180` at addresses 1, 2, 3, 4
3. Then `chip_ver_get` should return `QCA_VER_MHT`, the device registers, and
   `qca-nss-dp` can go back in
4. Only then: does the QCA8386 forward by default, and the socket-to-port map
