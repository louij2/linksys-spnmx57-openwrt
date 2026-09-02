# What is left to get OpenWrt fully working

Written 2026-09-02, from the vendor device tree
(`collected/vendor/vendor-palm15.dts`), mainline `ipq5018.dtsi`, and
`ipq5018-mx-base.dtsi` from the fanchmwrt qualcommax tree. Everything below is
checked against one of those three rather than assumed.

The headline: **Ethernet is the only large piece of work.** Most of the rest is
either already provided by the shared base dtsi, or is confirmed to not exist
on this board.

---

## 1. Ethernet. The blocker

Covered in `investigation.md`. Three stages, in order, and stage 3 is the one
that decides whether this project succeeds.

### 1a. Answer the clock liveness question

`docs/bench-test.md`. One MDIO read on the serial console. Needs no build.
Decides whether the device tree work alone is enough, or whether the driver
needs reordering as well.

### 1b. Build the candidate

Backport the mainline QCA8084 series (patches `0301`-`0312` in `qualcommbe`) to
whichever tree the flashed image came from, set `CONFIG_IPQ_NSSCC_QCA8K=y`, and
build `dts/ipq5018-spnmx57.dts`. Boot it from RAM over TFTP, not from flash.

Expected first result: the `-22` goes away and the package probes. Work the
`[GUESS]` markers in the DTS in the order they are flagged if it does not.

### 1c. Find out whether the QCA8386 forwards by default

**This is the real risk, not the `-22`.** Mainline has no QCA8386 driver.
`qca8k` covers the QCA8327 and QCA8337 only.

If the switch comes out of reset forwarding, you get one interface with four
working sockets behind it and the job is done. If it does not, this stops being
device tree work and becomes either register poking with no reference to copy,
or an SSDK port.

Nobody has checked. One boot answers it.

---

## 2. Already handled by `ipq5018-mx-base.dtsi`, for free

The candidate DTS includes that file, and it turns out to describe this board
correctly. Every value below was cross checked against the vendor tree and
matches **exactly**:

| Thing | Base dtsi | Vendor tree | Match |
|---|---|---|---|
| WPS button | `&tlmm 27 GPIO_ACTIVE_LOW` | `gpios = <&tlmm 0x1b 0x01>` | yes |
| Reset button | `&tlmm 28 GPIO_ACTIVE_LOW` | `gpios = <&tlmm 0x1c 0x01>` | yes |
| LED red | `<&pwm 3 1250000>` | `pwms = <&pwm 0x03 0x1312d0>` | yes |
| LED green | `<&pwm 0 1250000>` | `pwms = <&pwm 0x00 0x1312d0>` | yes |
| LED blue | `<&pwm 1 1250000>` | `pwms = <&pwm 0x01 0x1312d0>` | yes |
| MDIO1 pins | gpio36 mdc, gpio37 mdio | same | yes |
| MAC address source | `devinfo` partition, `hw_mac_addr` | not in vendor DT | n/a |

The RGB status LED is worth calling out because it looked like a gap: there is
no PWM node in mainline `ipq5018.dtsi`, so on a mainline-only tree the LEDs
would need a driver. The fanchmwrt qualcommax tree already carries `&pwm`, and
the base dtsi wires all three channels with the right periods. **Nothing to do**
provided you build in a tree that has it.

Also free: **thermal**. `qcom,ipq5018-tsens` with five sensors and its
calibration fuses is in mainline `ipq5018.dtsi` already.

---

## 3. Confirmed to not exist on this board. No work at all

From the vendor tree's own `status` properties:

| Node | Vendor status | Meaning |
|---|---|---|
| `usb3@8A00000` | `disabled` | no USB port on this board |
| `pci@80000000` (domain 0, 1 lane) | `disabled` | second PCIe unused |
| `wifi1@c000000`, `wifi2@c000000` (QCN6122) | `disabled` | no extra radio cards |
| `wifi4@f00000`, `wifi5@f00000` (QCN9224) | `disabled` | ditto |
| `bt@7000000` | reserved memory only | integrated BT, not a usable device |

Do not spend time on any of these.

---

## 4. Verified while writing this, worth recording

The candidate DTS inherits `&pcie0` and `perst-gpios = <&tlmm 15>` from the
SPNMX56. That looked like an inherited assumption, so it was checked:

- Mainline `ipq5018.dtsi` labels the controllers `pcie1: pcie@80000000` and
  **`pcie0: pcie@a0000000`**, which is the opposite way round to what the
  addresses suggest
- The vendor tree enables `pci@a0000000` (2 lanes, `perst-gpio` gpio15) and
  disables `pci@80000000`
- The phy phandles agree too: `pcie0_phy: phy@86000` in mainline, and
  `phys = <0x1f>` on `pci@a0000000` resolves to vendor `phy@86000`

So `&pcie0` plus gpio15 is **correct** for this board. That is now checked
rather than inherited.

---

## 5. Not yet known, and `scripts/collect.sh` answers most of it

These need the flashed unit over Wi-Fi. None are hard, they are just unmeasured.

### 5a. Is the 2.4 GHz radio actually up?

The IPQ5018 has an integrated radio, and the vendor enables it:

```
wifi@c000000 {
        compatible = "qcom,cnss-qca5018", "qcom,ipq5018-wifi";
        qcom,tgt-mem-mode = <0x01>;
        status = "ok";
};
```

The 5 GHz QCN9074 on PCIe is separately enabled as `wifi3@f00000`
(`qcom,cnss-qcn9000`, `board_id = <0xa0>`).

"Wi-Fi works" has been recorded for this device, but it is not clear whether
that means **both** radios or only the PCIe one. `iw dev` and `dmesg | grep
ath11k` settle it in seconds. If only one radio is up, the internal one needs
`&wifi` enabled with `qcom,ath11k-fw-memory-mode = <1>` (which the candidate
DTS already does) plus a calibration variant that exists in
`firmware_qca-wireless`.

### 5b. Does an ath11k calibration variant exist for the SPNMX57?

The candidate DTS asks for `qcom,ath11k-calibration-variant = "Linksys-SPNMX57"`
on both radios. If no matching entry has been contributed to
`openwrt/firmware_qca-wireless`, ath11k falls back to a generic board file and
the radios come up with wrong or conservative regulatory and power settings.
Check what the flashed unit is actually loading before assuming it is fine.

### 5c. MAC addresses

The base dtsi reads them from the `devinfo` partition via an
`ascii-eq-delim-env` nvmem layout. The vendor tree does not describe this at
all (it has a placeholder `local-mac-address = [00 00 00 00 00 00]`), so it
cannot corroborate the offsets. Confirm the interfaces come up with the MACs
printed on the label rather than random ones.

### 5d. Which tree the flashed image was built from

`DISTRIB_TARGET` from `/etc/openwrt_release`. This decides where the QCA8084
backport goes and whether `&pwm` is available. It is the single most useful
line `collect.sh` returns right now.

### 5e. Flash description

The vendor tree describes **parallel** NAND (`qcom,nandcs`, `ebi2-nandc`)
while the base dtsi describes **SPI NAND** (`compatible = "spi-nand"`). Since
UBI and the overlay already work on the flashed unit, the SPI NAND description
is evidently the correct one and the vendor's is more AP-MP03.1 reference board
boilerplate. Worth confirming with `/proc/mtd` rather than leaving as an
inference.

---

## 6. Once Ethernet works: making it a real port

None of this is worth starting before stage 1c is answered.

- **Image recipe.** A device entry in the target `image/` makefile, correct
  `DEVICE_DTS`, `SOC`, and the Linksys image format so `sysupgrade` produces a
  flashable image
- **`board.d` network defaults.** With one interface and a dumb switch behind
  it, the default config is not the usual `lan` bridge plus separate `wan`.
  Decide what a sensible out of box config is: most likely everything on `lan`,
  with WAN over VLAN or over one physical socket if the switch can be told to
  separate one
- **Port to socket mapping.** The vendor tree gives `port_id` values but no
  labels, so which of `port1..port4` is the socket marked WAN on the case is
  unknown. Plug a cable in and watch which one links
- **Which ports are 2.5G.** The product page says two 2.5G and two 1G. The
  QCA8084 is a quad 2.5G PHY, so this is likely a magnetics or marketing limit
  rather than anything describable. Measure it
- **Dual boot / recovery.** Work out what the stock partition layout offers for
  getting back to vendor firmware, and document it
- **Upstream.** Submit the DTS, the image recipe, and the calibration variant.
  The forum thread is the right place to start

---

## Suggested order

1. `docs/bench-test.md` on the serial console, and check U-Boot for `tftpboot`
   plus `bootm` while you are there
2. `scripts/collect.sh` over Wi-Fi, mainly for `DISTRIB_TARGET` and whether
   both radios are up
3. Backport, build, boot from RAM, kill the `-22`
4. Answer the QCA8386 forwarding question. **Everything after this depends on
   it**
5. Only then: image recipe, network defaults, port mapping, upstream
