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

## From stock

**Still not achieved.** Nobody has an SPNMX57 that went stock -> OpenWrt. The
author's unit was already running OpenWrt when this port began, so it arrived
by `sysupgrade`. `factory.bin` is built and is the right shape for the job, but
every OEM route tried so far is closed. What follows is what was actually
measured on a second, never-opened unit, so the next person does not repeat it.

### The vendor web UI is closed by design on ISP units

The Linksys Smart Wi-Fi UI decides whether to show you anything with this
(from the device's own `shared-util.js`):

```js
IsAdminPasswordDefault -> false ? "configured"
                       -> true  ? ask nodes/setup/IsAdminPasswordSetByUser
```

So "configured" means nothing more than *the admin password is not the generic
default*. An ISP unit ships with a unique per-device password on its label,
which is never the default, so the UI classifies a factory-fresh unit as
configured and redirects to an app-download page. **A factory reset cannot
clear this**, because the reset restores that same per-unit password.

Measured on `MX57CF` / "Velop Pro 6 SP", firmware `1.0.1.216553`:

| probe | result |
|---|---|
| `core/GetDeviceInfo` (no auth) | full device info, works |
| `core/IsAdminPasswordDefault` | `false` |
| `nodes/setup/IsAdminPasswordSetByUser` | `false` (nobody ever set one) |
| `/cgi-bin/upload.cgi` and friends, authenticated | `403` |
| UI in setup mode | moves to port **52000**; JNAP is not served on port 80 |

The `403` on every candidate upload path, rather than `401` or `404`, is the
discouraging one. Note also that lighttpd returns `401` for *any* `*.cgi` path
before checking whether it exists, so unauthenticated probing tells you
nothing. Verify that with a made-up filename before reading anything into it.

### `factory.bin` has no vendor header

```
00000000: d00d feed ...   "ARM64 OpenWrt FIT (Flattened Image Tree)"
```

It is a bare FIT. Whatever validates an upload on the vendor side has nothing
to recognise. Making the OEM route work most likely means understanding the
header on the official image
(`FW_MX57CF_1.0.1.216553_prod.img`, md5 `62b76e25b194ecd42275460a7eedcace`)
and wrapping the FIT to match. Nobody has done that yet.

### The stock firmware wedges after a factory reset

Seen repeatably: after a reset the unit serves its setup pages for a short
while, then **every userspace service dies while the kernel stays up**. It
pings with no loss, ARP is fine, the link stays at 1000baseT, and ports 80,
443 and 52000 all stop answering. It does not recover on its own; it was still
dead after eight minutes of sampling at five second intervals.

Two consequences worth knowing:

- **The reset button stops working too.** It is read by a userspace daemon, so
  in this state you can hold it as long as you like and nothing happens. Only
  pulling the power recovers the unit. If a reset "does not take", check
  whether the unit is answering TCP at all before concluding you pressed the
  wrong button.
- Watch the **link state**, not the LED, to confirm a reboot actually happened.
  A power cycle drops the link; a button press on a wedged unit does not.

### U-Boot TFTP, the route that should work

Needs UART. Interrupt autoboot, then:

```
setenv ipaddr <a free address on your LAN>
setenv serverip <your TFTP server>
tftp 0x44000000 factory.bin
```

Use `setenv`, never `saveenv`, so nothing persists if it goes wrong.

### A trap when testing on a directly-attached machine

A reset router comes up on `192.168.1.1`. If your machine has a VPN carrying a
subnet route for `192.168.1.0/24`, that route can win and your probes will
quietly reach a different site entirely. Check with `route get 192.168.1.1`
before trusting any result, and pin it with a host route if needed. Check your
interface priority too: an ethernet service ordered above Wi-Fi will take the
default route from a router that has no internet behind it.

If you get further than this, please report it on the
[forum thread](https://forum.openwrt.org/t/openwrt-support-for-linksys-spnmx57-variants/231653)
so this section can become a real procedure.

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
