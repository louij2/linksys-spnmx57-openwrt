**SPNMX57 progress: it boots, Wi-Fi works, Ethernet doesn't yet — and I think I know why**

Short version: the `-22` everyone has been chasing is a red herring on `qualcommax`.
The real blocker is that nothing in OpenWrt ever brings the QCA8084 package up, so
the PHYs are silent and the SSDK can't detect the switch.

---

**1. You can get the vendor device tree without any hardware**

The stock image is a public download and is itself a u-boot FIT, so `dtc` reads it:

    curl -fLO http://download.linksys.com/updates/20250403t130837/FW_MX57CF_1.0.1.216553_prod.img
    # md5 62b76e25b194ecd42275460a7eedcace
    dtc -I dtb -O dts -o fit.dts FW_MX57CF_1.0.1.216553_prod.img

`fdt@1` is uncompressed, so the inner blob is just the second `d00dfeed` (offset
`0x499c30`, totalsize 43034). Board codename is **Palm15**.

**2. The real topology**

    IPQ5018 MAC1 --UNIPHY1-- SGMII+ 2.5G forced --> QCA8386
                                                     port0 = CPU
                                                     port1..4 = QCA8084 EPHYs @ MDIO 1,2,3,4

Not what the thread assumed. `switch_mac_mode = 0x0c` = `MAC_MODE_SGMII_PLUS` (not
QXGMII). `switch_wan_bmp = 0` — no hardware WAN port. Only **one** `nss-dp` node
exists (`dp1`, `qcom,id = 2`); MAC0's GE PHY is real silicon but drives no socket.
Nothing at `0x1c` — that was the 56's QCA8081.

**3. Two routes, and mainline is the wrong one here**

The mainline `qcom,qca8084-package` binding needs a `qca8084-nsscc` clock
controller, which needs PCS/uniphy parent clocks that exist on IPQ9574
(`qualcommbe`) and **do not exist on `qualcommax/ipq50xx`**. Dead end on this
target. (Note master has since moved qualcommax to stmmac/DSA + a `qca-uniphy` PCS
driver on 6.18 — that may change the answer later.)

The SSDK route is right: `openwrt/qca-ssdk` already contains full MHT
(QCA8084/QCA8386) support — `ssdk_mht.c`, `ssdk_mht_clk.c`, `hsl/mht/*`.

**4. MHT is compiled out — fixed, and it works** ← actionable

`package/kernel/qca-ssdk/Makefile` passes `MHT_ENABLE=disable` for every subtarget,
and qca-ssdk's own `config` only auto-enables MHT for ipq53xx/ipq95xx/ipq60xx.
Verified on the running unit:

    strings /lib/modules/6.12.57/qca-ssdk.ko | grep -ci mht   ->  0

So `regi_init`'s `case CHIP_MHT:` was an empty break. Adding to the ipq50xx block:

    MAKE_FLAGS+= CHIP_TYPE=MP MHT_ENABLE=enable ISISC_ENABLE=enable IN_QCA808X_PHY=TRUE

(MAKE_FLAGS is one command line, so these later assignments beat the blanket
`MHT_ENABLE=disable` above.) Result: **303** MHT strings, and a line that never
appeared before:

    ssdk_dt_parse[1446]:INFO:switch node is qca8386!

**4b. That build needs an upstream fix to qca-ssdk** — likely affects anyone
enabling MHT on a 6.12 target, not just this board. `IN_QCA808X_PHY=TRUE` pulls in
`src/hsl/phy/qca808x.c`, whose `match_phy_device` still uses the pre-6.12
one-argument form; 6.12 added `const struct phy_driver *`. Build dies with
`-Wincompatible-pointer-types` at the `.match_phy_device` initialiser. Fix:

    #if (LINUX_VERSION_CODE >= KERNEL_VERSION(6,12,0))
    int qca808x_match_phy_device(struct phy_device *phydev,
                    const struct phy_driver *phydrv)
    #else
    int qca808x_match_phy_device(struct phy_device *phydev)
    #endif

**5. The actual chicken-and-egg**

`chip_ver_get` → `qca_detect_phyid` does a plain C22 read of regs 2/3 at the
`phy_address` values from the DTS. I confirmed on hardware that SSDK issues
exactly 16 reads, 4 each at addresses 1/2/3/4 — bus and addresses are correct.
The chip doesn't answer because the EPHY addresses come from a strap register
(`EPHY_CFG`, `0xC90F018`) that **qca-ssdk only ever reads, never writes**
(`qca_mht_ephy_addr_get`, one read, no writes anywhere in the tree).

QSDK does it in the **MDIO driver**: `mdio-qca.c` binds `qcom,qca-mdio` and calls
`qca_mht_preinit()` at probe, programming EPHY/UNIPHY addresses from
`phyaddr_fixup = <0xC90F018>` / `uniphyaddr_fixup = <0xC90F014>` and running the
clock init when `mdio_clk_fixup` is present. OpenWrt binds mainline
`drivers/net/mdio/mdio-ipq4019.c` on `qcom,ipq5018-mdio`, which has **zero**
occurrences of fixup/8084/8386/mht. So nothing ever straps or clocks the package,
and SSDK's own MHT clock code runs only *after* detection succeeds.

**6. Two DTS gotchas that cost me boots**

- `&mdio0` must be `status = "okay"` even though MAC0 drives no socket. SSDK's
  `ssdk_dt_get_mdio_node()` finds its default bus by compatible string, and
  `mdio@88000` is the only node carrying `qcom,ipq40xx-mdio`. Disabled node → no
  platform device → `cannot find platform device from mdio node`.
- `&switch` must keep `port@0 { port_id = <1>; mdiobus = <&mdio0>; phy_address = <7>; }`.
  On ipq5018 `chip_ver_get` falls through to `chip_is_scomphy()` → `qca_detect_phyid(0)`,
  which reads that PHY. Remove it and **device 0** fails with `-ENODEV` too.

**6b. The bus map — measured, not inferred**

With MHT compiled in I put explicit `ethernet-phy@N { reg = <N>; }` children for
all 32 addresses on `&mdio1`, so the kernel probes the bus itself before qca-ssdk
loads. Result on a cold chip:

| addr | phy_id |
|---|---|
| `0x00`-`0x03` | missing (`0xffff`) |
| `0x04`, `0x05` | `0x00000000` |
| `0x06`-`0x0f` | missing |
| `0x10`-`0x13` | `0x00000000` |
| `0x14`-`0x1f` | `0xb00eb00e` |

Control on `mdio@88000` addr 7: `0x004dd0c0`, valid.

**No valid PHY ID anywhere** (QCA8084 would be `0x004dd180`). A `0xb00eb00e`
repeating identically in regs 2 and 3 is not a device. But the structure — some
`0xffff`, some `0x0000`, some a repeating word — suggests the chip is present and
partially responsive, just uninitialised. This is the direct measurement behind
point 5: the EPHYs really are unreachable at 1-4 or anywhere else until something
writes the strap.

This scan is also the cheap success test for anyone attempting the port: it should
show `0x004dd180` at addresses 1, 2, 3, 4.

**7. Status**

Image builds, flashes (`sysupgrade -F`, board name mismatch needs it), boots as
`Linksys SPNMX57`, both Wi-Fi radios work, LuCI up, stays reachable. Ethernet not
working. `qca-nss-dp` is currently removed from my debug image because it
initialises against the device SSDK failed to register and panics in
`fal_mib_port_flush_counters`.

Recovery is reliable and measured: U-Boot `auto_recovery=yes`, `boot_part_ready=3`
— three boot attempts, flips slot on the 4th power-on. `sysupgrade` writes the
inactive slot, so a bad image costs you nothing but power cycles. Leave
`linksys,spnmx57` **out** of `/etc/init.d/bootcount` while testing and the image
self-reverts.

**What's left**

Just one thing now: **port `qca_mht_preinit()` (address strap + clock init) into
`mdio-ipq4019.c`**, gated on the `*_fixup` DT properties. Everything else is done
and measured. This is where help would count most — if anyone has already done
this, or knows the QSDK `mdio-qca.c` path well, please say.

After that: does the QCA8386 forward by default, and what's the socket-to-port map.

Repo with the vendor DTB, all boot logs, the DTS and the patches:
https://github.com/louij2/linksys-spnmx57-openwrt

Happy to share the full write-up, vendor DTB, boot logs and DTS if useful.
