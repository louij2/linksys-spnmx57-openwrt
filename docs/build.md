# Building it, and a course correction

## The short version

The first candidate DTS took the **mainline QCA8084 PHY package** route, because
that is what the upstream patch series and the `-22` error message point at.
**That route is wrong for this target.** The right one was already sitting in
the tree.

The old candidate is kept at
`dts/attic/ipq5018-spnmx57-mainline-phy-package.dts` so the reasoning is
traceable. Do not build it.

## Why the mainline route is wrong here

The flashed unit runs **`qualcommax/ipq50xx`**, OpenWrt SNAPSHOT
`r31810-05feabfd09`, Linux 6.12.57. That target drives Ethernet with the
downstream **`nss-dp` + `qca-ssdk`** stack, not with mainline DSA/PPE.

Working through the backport surfaced a hard blocker. The mainline
`qcom,qca8084-package` binding requires a `qca8084-nsscc` clock controller,
and that controller needs seven parent clocks:

```
clocks = <&pcs0_pll>,
         <&qca8k_uniphy0_rx>, <&qca8k_uniphy0_tx>,
         <&qca8k_uniphy1_rx>, <&qca8k_uniphy1_tx>,
         <&qca8k_uniphy1_rx312p5m>, <&qca8k_uniphy1_tx312p5m>;
```

Those come from the **PCS/uniphy clock provider** that exists on IPQ9574 in
`qualcommbe`. On `qualcommax/ipq50xx` there is no such provider and no such
driver. Checked directly: the only occurrences of `qca8k_uniphy1_tx312p5m`
anywhere in `qualcommax` were inside the binding document itself.

So the mainline route needs, on top of the eleven PHY patches, a PCS uniphy
driver ported to IPQ5018 whose hardware differs from the IPQ9574 one. That is a
large piece of work to reimplement something the target already has.

## What the target already has

`package/kernel/qca-ssdk` builds from `https://github.com/openwrt/qca-ssdk.git`,
pinned at `446db12b1fd3bca2e61e45cb01c4ad52cfddd95b` (2025-05-30). That tree
already carries full **MHT** support, which is Qualcomm's name for the
QCA8084/QCA8386 part:

| file | what it does |
|---|---|
| `src/init/ssdk_mht.c` | `qca_mht_hw_init()`, `qca_mht_work_mode_init()`, `qca_mht_interface_mode_init()`, `qca_mht_mdio_master_init()`, `qca_mht_portctrl_hw_init()` |
| `src/init/ssdk_mht_clk.c` | the whole clock tree: assert, deassert, enable, disable, parent set, rate set, uniphy raw clock set |
| `src/hsl/mht/mht_sec_ctrl.c` | `qca_mht_ephy_addr_get()`, which reads the EPHY addresses out of the `EPHY_CFG` strap register |

These are the same files Hyndland cited from the vendor GPL drop. They are not
a vendor blob to be ported: **they are already compiled into this target and
already loaded on the flashed unit**, which is why `/sys/ssdk/` exists on it and
why the boot log says `qca-ssdk module init succeeded`.

`src/init/ssdk_dts.c` parses `qcom,ess-switch-qca8386` and `switch_mac_mode1`
directly, and supports the `ess-switch<N>` naming for a second device.

So the clock and strap bring-up that the mainline PHY package driver
reimplements is already present. **The board only ever needed describing.**

## What that means for the `-22`

The `-22` is a real bug in the mainline route, and Hyndland's analysis of it
stands. It is just not the bug that has to be fixed to get this board working
on `qualcommax`. On this target the QCA8084 PHY package driver should never be
bound at all; the SSDK owns the chip.

## The current candidate

`dts/ipq5018-spnmx57.dts`, written in SSDK style from the vendor tree:

- `&ess_instance` gets `num_devices = <2>` and a second switch,
  `ess-switch1@1`, `compatible = "qcom,ess-switch-qca8386"`, reached over
  `mdio-bus = <&mdio1>`
- `switch_mac_mode = <MAC_MODE_SGMII_PLUS>` and
  `switch_mac_mode1 = <MAC_MODE_DISABLED>`, straight from the vendor's `0x0c`
  and `0xff`
- CPU port 0 forced 2500/full, ports 1 to 4 with `phy_address` 1 to 4
- `&switch` (the SoC side) also set to `MAC_MODE_SGMII_PLUS`, with only
  `port@1` (`port_id = 2`, forced 2500), because MAC0 has no `nss-dp` node
- `&dp2` enabled and labelled `lan`; `&dp1` left disabled
- `&mdio1` reset moved to **gpio24**, the QCA8386 package reset, rather than
  the 56's gpio39

Wi-Fi deliberately keeps the **SPNMX56** calibration variant. It is known good
on this exact hardware, and no SPNMX57 variant exists in
`firmware_qca-wireless`, so asking for one would silently fall back to a
generic board file. One variable at a time.

## Build host

`srv-openstack`, aarch64, 80 cores, 605G free, Ubuntu 26.04. Kept nice'd and
job-capped so it cannot disturb the OpenStack services sharing the box.

```
git clone https://github.com/openwrt/openwrt.git
git checkout 05feabfd09          # exactly what the flashed unit runs
# add ipq5018-spnmx57.dts and the linksys_spnmx57 recipe in image/ipq50xx.mk
CONFIG_TARGET_qualcommax=y
CONFIG_TARGET_qualcommax_ipq50xx=y
CONFIG_TARGET_qualcommax_ipq50xx_DEVICE_linksys_spnmx57=y
```

## Two things this does not settle

1. **Whether the SSDK's MHT path is reached at all.** It has to be selected by
   the `ess-switch1` node being parsed and `num_devices = <2>` being honoured.
   That is the first thing to check in the boot log
2. **Port to socket mapping.** Still unknown, and still needs a cable and a
   look at which port lights up

---

# BUILD RESULT, 2026-09-02

**It builds.** Three images, from OpenWrt `05feabfd09` plus the SPNMX57 DTS and
image recipe:

| image | size | use |
|---|---|---|
| `...-initramfs-uImage.itb` | 14.5 MB | **RAM bootable.** Test without flashing |
| `...-squashfs-sysupgrade.bin` | 14.8 MB | flash from a running OpenWrt |
| `...-squashfs-factory.bin` | 19.7 MB | flash from stock |

LuCI and uhttpd are included, so the flashed unit gets a web UI without
fighting `apk` snapshot drift.

## The built DTB, verified against the vendor tree

Decompiled the actual compiled `.dtb` rather than trusting the source. Every
value below matches `collected/vendor/vendor-palm15.dts` exactly:

| property | built | vendor |
|---|---|---|
| `model` | `Linksys SPNMX57` | n/a |
| `num_devices` | `0x02` | `0x02` |
| `ess-switch1@1` compatible | `qcom,ess-switch-qca8386` | same |
| `switch_mac_mode` (both) | `0x0c` | `0x0c` |
| `switch_mac_mode1` | `0xff` | `0xff` |
| `switch_cpu_bmp` | `0x01` | `0x01` |
| `switch_lan_bmp` | `0x1e` | `0x1e` |
| `switch_wan_bmp` | `0x00` | `0x00` |
| QCA8386 port0 | forced `0x9c4`/duplex 1 | same |
| QCA8386 ports 1-4 | `phy_address` 1,2,3,4 | same |
| SoC port_id 2 | forced `0x9c4`/duplex 1 | same |
| `mdio@90000` reset | `<tlmm 0x18 1>` = gpio24 active low | gpio24 |
| `dp1` | `disabled` | no nss-dp node |
| `dp2` | `okay`, label `lan` | `qcom,id = 2` |

## Three build failures worth recording

1. **gcc 15 / glibc 2.42.** srv-openstack runs Ubuntu 26.04, and OpenWrt's
   bundled gnulib will not compile against it:
   `./stdlib.h:827:20: error: expected identifier or '(' before '_Generic'`.
   Fixed by building inside a Debian 12 container (`owrt-deb12`)
2. **Feed drift.** Pinning the core to the device's Nov 2025 commit while
   pulling current feeds gives LuCI 2026 wanting `ucode >= 2026.02.27` against
   a `2025.11.07` core. Fixed by pinning the luci feed to
   `9717162fd030ba8a3b0adad0af330648ef9fe3e1` (2025-11-12)
3. **Stale artifacts beat the pin.** After pinning, `bin/packages/.../luci/`
   still held the old `~506ca60` apks and apk kept selecting them. The fix is
   to delete that directory, not just re-run `feeds update`

## Worth knowing: master has moved on

`qualcommax` on master is now kernel **6.18** with mainline **stmmac + DSA**, a
new **`qca_ppe`** driver, and a **`pcs: qca-uniphy`** driver. The SSDK is gone
from that target.

That last one matters: the PCS uniphy clock provider whose absence killed the
mainline QCA8084 route on the pinned tree **now exists upstream**. QCA8084
support itself still does not, and the SSDK-style DTS here is invalid on
master, so this is the future direction rather than today's build. Revisit once
this image has proved the topology is right.
