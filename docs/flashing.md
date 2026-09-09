# Flashing the SPNMX57

> [!WARNING]
> This is an unofficial port proven on **one unit**. Going from stock firmware
> to OpenWrt has **never been done on this device** — see
> [From stock](#from-stock-untested). Have a UART attached before you try it.

The original device-specific lab notes are kept at
[flashing-lab-notes.md](flashing-lab-notes.md) for provenance. They contain the
author's own IP addresses and are not a procedure to follow.

## Before you start

**Get a UART.** On this device it is not optional comfort, it is the recovery
path. See [Serial console](#serial-console).

**Know which image you need:**

| you are on | use | how |
|---|---|---|
| OpenWrt already | `...-squashfs-sysupgrade.bin` | `sysupgrade` |
| stock ISP firmware | `...-squashfs-factory.bin` | OEM web UI or U-Boot TFTP |

**Never push `factory.bin` through `sysupgrade`.** It carries no fwtool
metadata, and forced through, its `d00dfeed` header makes `nand_do_flash_file`
dispatch to `nand_upgrade_fit` and write the whole ~19.7 MB into the kernel
volume. That is a brick that only UART recovers.

Verify your download against the `sha256sums` file on the release before
flashing anything.

## Dual firmware, and the recovery net

The vendor U-Boot keeps **two complete firmware slots** and picks between them
with the `boot_part` variable:

| `boot_part` | kernel | rootfs |
|---|---|---|
| 1 | `mtd12` `kernel` | `mtd13` `rootfs` |
| 2 | `mtd14` `alt_kernel` | `mtd15` `alt_rootfs` |

`sysupgrade` writes to the slot you are **not** running from and flips
`boot_part`, so a failed flash leaves the previous firmware intact in the other
slot. You can switch back by hand from U-Boot:

```
setenv boot_part 1    # or 2
saveenv
reset
```

That is the single most useful recovery fact about this device.

### The auto-recovery caveat — read this

U-Boot increments a boot counter each boot and, with `auto_recovery=yes`, flips
`boot_part` if userspace never clears it. This build **does** clear it
(`mtd resetbc s_env`, via `/etc/init.d/bootcount`), which is correct upstream
behaviour and stops the firmware spuriously reverting.

The consequence is that **auto-recovery only rescues you from a device that
never reaches userspace.** If it boots far enough to run init but the network
does not come up, U-Boot will not flip slots for you and there is no
power-cycle trick. You recover from UART, or by setting `boot_part` by hand.

Older notes in this repo describe a "4 power cycles and it flips back" escape.
That no longer applies.

## From OpenWrt (sysupgrade)

```bash
# dropbear has no sftp-server, so scp fails - pipe it instead
ssh root@<device> 'cat > /tmp/sysupgrade.bin' < openwrt-...-squashfs-sysupgrade.bin
ssh root@<device> 'sha256sum /tmp/sysupgrade.bin'   # compare against the release
ssh root@<device> 'sysupgrade -n /tmp/sysupgrade.bin'
```

`-n` discards existing config. Drop it to keep settings, but expect to re-check
network config afterwards since this port changes the interface layout.

Watch it on the UART. It reboots itself; do not power-cycle during the write.

## From stock (UNTESTED)

**Nobody has done this.** The author's unit was already running OpenWrt when
this port began, so it arrived by `sysupgrade`. `factory.bin` is built and is
the correct image for the job, but the path is unproven.

Two candidate routes, in order of preference:

1. **OEM web UI firmware update** with `factory.bin`. Whether the vendor UI
   accepts an unsigned image is unknown.
2. **U-Boot TFTP.** Interrupt autoboot on the UART, then:
   ```
   setenv ipaddr <a free address on your LAN>
   setenv serverip <your TFTP server>
   tftp 0x44000000 factory.bin
   ```
   Use `setenv`, never `saveenv`, so nothing persists if it goes wrong.

If you attempt this, please report the result on the
[forum thread](https://forum.openwrt.org/t/openwrt-support-for-linksys-spnmx57-variants/231653)
so this section can be replaced with a real procedure.

## Serial console

The console runs at **115200 8N1** and drops you at a **root shell with no login
prompt**, which is what makes it a complete recovery path.

The board has a UART header. **This document deliberately does not print a pin
map**, because a wrong one can damage the board or your adapter, and only one
unit has been opened. Identify it yourself:

- Use a **3.3 V** USB-serial adapter. Not 5 V.
- **Do not connect VCC.** Connect GND, TX and RX only, and power the router from
  its own supply.
- Find GND with a multimeter against the shield or a known ground.
- TX idles high at 3.3 V; RX floats. If you get nothing, swap TX and RX — that
  is harmless and is the usual mistake.

Once connected, U-Boot autoboot is interruptible with a keypress and the prompt
is `IPQ5018#`. Note U-Boot here uses `tftp`, **not** `tftpboot`.

## Non-destructive testing

You can boot a whole firmware from RAM without touching NAND, which is how this
port was developed. `newstack/tools/ramboot-test.py` automates it: it reboots
over serial, interrupts U-Boot, TFTPs the initramfs and `bootm`s it. Nothing is
written to flash and a power cycle returns you to the installed firmware.

That is the right way to try a build before committing it.
