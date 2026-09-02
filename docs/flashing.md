# Flashing, and the five defects the pre-flight caught

## The image was NO_GO on the first build

A pre-flight verification (four independent read-only checks, each attacked by
three skeptics) returned **NO_GO**. The DTS and image recipe had landed, but the
**userspace board plumbing had not**: `grep -rIl spnmx57` over the built rootfs
returned **zero files**.

Flashing that build would very likely have lost the device.

| file | consequence of the missing `spnmx57` case |
|---|---|
| `etc/hotplug.d/firmware/11-ath11k-caldata` | **Fatal.** This derives the radio MACs from `devinfo hw_mac_addr`. No case means no `caldata_extract`, no `ath11k_patch_mac`, no `ath11k_set_macflag`. The `cal-*.bin` blobs live only on the overlay, are in no `keep.d`, and `/etc/sysupgrade.conf` is empty, so they do not survive. **Wi-Fi is the only way in, so this loses the device** |
| `etc/init.d/bootcount` | `mtd resetbc s_env` never runs, so the U-Boot counter is never cleared and the bootloader flips back to the old slot on the 4th power-on **even if the image works perfectly**. Self-reverting |
| `lib/upgrade/platform.sh` | The new image cannot sysupgrade itself. It falls through to `default_do_upgrade` with `PART_NAME=firmware`, a partition that does not exist here |
| `etc/board.d/02_network` | No lan/wan definitions at all on a factory reset |
| `etc/hotplug.d/ieee80211/05-wifi-migrate` | Wi-Fi config migration skipped |

All five are fixed and **verified inside the shipped rootfs**, unsquashfs'd out
of the `.bin` rather than read from `build_dir`.

`02_network` deliberately gets its **own** case rather than joining the 56's
list, because the topology differs:

```sh
linksys,spnmx57)
        # The QCA8386 fronts four QCA8084 PHYs and hangs off a single
        # forced 2.5G link, and switch_wan_bmp is 0, so there is one
        # netdev and no hardware WAN port.
        ucidef_set_interface_lan "lan"
        ;;
```

## Recovery is armed, and confirmed three ways

- Live U-Boot env: `auto_recovery=yes`, `boot_part=2`, `boot_part_ready=3`
- A disassembly of CBT U-Boot 7.3.21 in `mtd6`
- 99 chronological boot-count records in `s_env` (`mtd10`)

**Threshold is exact: three boot attempts, and the 4th power-on flips slots.**

`sysupgrade` writes the **inactive** slot (`mtd12` kernel + `mtd13` rootfs) and
sets `boot_part=1`, leaving the working SPNMX56 install untouched in `mtd14` /
`mtd15`. So the flash is genuinely reversible by failure.

The catch: **the flip only fires on a real power cycle.** A kernel that hangs
without rebooting sits there until someone cuts power. Four cycles are needed
in the worst case: three that boot the bad slot, and the fourth that flips.

## The exact procedure

Verified working up to the final step.

```sh
# 1. stage it (dropbear has no sftp-server, so scp fails; pipe instead)
ssh root@10.0.0.68 'cat > /tmp/spnmx57.bin' < openwrt-...-squashfs-sysupgrade.bin

# 2. confirm the three recovery preconditions read exactly this
ssh root@10.0.0.68 'fw_printenv | grep -E "^(auto_recovery|boot_part|boot_part_ready)="'
#   auto_recovery=yes   boot_part=2   boot_part_ready=3

# 3. dry run. Writes nothing, builds no backup archive
ssh root@10.0.0.68 'sysupgrade -T -F /tmp/spnmx57.bin'
#   expect: "Device linksys,spnmx56 not supported by this image"
#           "Image check failed but --force given - will update anyway!"

# 4. back the config off the device first
ssh root@10.0.0.68 'sysupgrade -b /tmp/backup.tar.gz'
ssh root@10.0.0.68 'cat /tmp/backup.tar.gz' > backup.tar.gz

# 5. flash
ssh root@10.0.0.68 'sysupgrade -F -v /tmp/spnmx57.bin'
```

## Flags: the rules are absolute

| flag | verdict |
|---|---|
| `-F` | **Required and sufficient.** The board reports `linksys,spnmx56`, the image declares `linksys,spnmx57`. The only failing check is the board-identity one, and `-F` defeats exactly that |
| `-n` | **Never.** There is no `owrt_ssid`/`owrt_wifi_key` in the U-Boot env and no `wlan.defaults` in `board.json`, so a regenerated `/etc/config/wireless` sets `disabled='1'` on **both** radios and emits no station section. No AP, no uplink, no IP, and Ethernet is dead. Guaranteed loss of access |
| `-s` | **Never.** Targets the currently booted slot and destroys the known-good fallback |
| factory.bin | **Never with sysupgrade.** It has no fwtool metadata; forced through, its `d00dfeed` header makes `nand_do_flash_file` dispatch to `nand_upgrade_fit` and write all 19.7 MB into the kernel volume. It is for the OEM web UI / TFTP only |

Ignore fwtool's own advice to "upgrade without keeping config". That message
belongs to the minor-compat branch, which is unreachable here, and following it
is precisely the action that loses the device.

## Why keeping config is safe

Both `wifi-device path=` values survive the DTS change byte for byte, verified
by decompiling the DTB out of the new image and diffing against the running one:

- `platform/soc@0/c000000.wifi` — the new DTS uses the label `&wifi0` instead of
  `&wifi`, but the emitted node is still `wifi@c000000` under `soc@0`
- `soc@0/a0000000.pcie/...` — `pcie@a0000000` keeps `linux,pci-domain = <0>` and
  `status = "okay"`; `pcie@80000000` stays disabled with domain 1, so the domain
  cannot shift on probe order

Same kernel, same ath11k, same `ipq-wifi-linksys_spnmx56` board file, same
`Linksys-SPNMX56` calibration variant. And `10.0.0.68` sits directly on
`phy1-sta0`, not on relayd (which is not even installed; `stabridge` is dead
config reporting `proto: none`), so the address does not depend on `br-lan`
coming up. `/etc/dropbear/authorized_keys` is in `keep.d`.

## Known residual risk

The preserved `/etc/config/network` declares
`network.@device[0].ports='lan1' 'lan2' 'lan3'`, names that will not exist under
the QCA8386 topology. netifd normally skips unresolvable bridge ports and
`br-lan` still comes up empty. Untested, and it is the most likely way to boot
successfully yet come back unreachable.

---

# The bootcount trap, and why this is TWO flashes

The adversarial pass caught a contradiction in the fix list above. It is the
single most important thing on this page.

## The trap

`/etc/init.d/bootcount` runs at `START=99` and does `mtd resetbc s_env`, which
clears U-Boot's boot counter. Adding a `linksys,spnmx57` case to it makes the
new image "stick" — and in doing so **destroys the recovery net that makes
flashing survivable at all**.

| scenario | bootcount **fixed** | bootcount **unfixed** |
|---|---|---|
| boots, Wi-Fi comes up | sticks permanently | works, but reverts on the 4th power cycle |
| boots, **Wi-Fi does not come up** | counter reset every boot, U-Boot **never flips**, device **permanently unreachable** | counter climbs, 4th power cycle flips, **recovered** |
| does not boot at all | counter climbs, 4th cycle flips, recovered | same, recovered |

The middle row is the one that matters. "Boots fine but comes back unreachable"
is the most likely failure here, because the preserved `/etc/config/network`
still lists ports (`lan1 lan2 lan3`) that do not exist under the QCA8386
topology. Fixing bootcount converts the one recoverable failure into an
unrecoverable one.

## So: test image first, permanent image second

### Stage 1 — `TEST-squashfs-sysupgrade.bin`

- caldata fix **kept** (this is what gives the radios their MACs, and Wi-Fi is
  the only way in)
- `platform.sh`, `02_network`, `05-wifi-migrate` fixes kept
- **bootcount fix deliberately omitted**, so the device rescues itself

Worst case: power cycle four times and you are back on the working SPNMX56
install. Nothing is lost.

### Stage 2 — `PERMANENT-squashfs-sysupgrade.bin`

Only after stage 1 is confirmed booted and reachable. Adds the bootcount case
so the image stops self-reverting.

## Two further gates found by the adversarial pass

Seven board-name gates exist in the shipped rootfs, not five. The two extra:

- `/etc/uci-defaults/30_uboot-envtools` — without a case there is no
  `/etc/config/ubootenv` and no `/etc/fw_env.config` on a clean install, so
  `fw_printenv`/`fw_setenv` stop working. Mitigated on a config-preserving
  upgrade by `keep.d/uboot-envtools`
- `/etc/apk/world` — cosmetic, records the board's package set

Neither is brick-grade. Both belong in the permanent image.

## Images and their checksums

| file | md5 | bootcount case | use |
|---|---|---|---|
| `TEST-squashfs-sysupgrade.bin` | `ae46a8598910048aaf127fb138f0f541` | absent, self-recovering | **flash this first** |
| `PERMANENT-squashfs-sysupgrade.bin` | `f9f276f940ea342801bc5f98cc7b316b` | present, sticks | flash only after stage 1 works |

The TEST image is already staged on the device at `/tmp/spnmx57.bin`, md5
verified, and its dry run passes.
