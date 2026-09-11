# Flashing the SPNMX57

> [!NOTE]
> This is an unofficial port, now proven on **two units**. Going from stock
> firmware to OpenWrt **works and needs no UART and no case opening** — see
> [From stock](#from-stock--proven-no-uart-no-case-opening). It flashes to the
> inactive slot, so the vendor firmware survives in the other one.

The original device-specific lab notes are kept at
[flashing-lab-notes.md](flashing-lab-notes.md) for provenance. They contain the
author's own IP addresses and are not a procedure to follow.

## Before you start

**You probably do not need a UART.** Stock to OpenWrt is done through the
vendor's own hidden `fwupdate.html` page, and `sysupgrade` handles OpenWrt to
OpenWrt. Both write to the inactive boot slot, so the previous firmware stays
intact. A UART is still the last-resort recovery path if you manage to damage
both slots — see [Serial console](#serial-console).

**Know which image you need:**

| you are on | use | how |
|---|---|---|
| OpenWrt already | `...-squashfs-sysupgrade.bin` | `sysupgrade` |
| stock ISP firmware | `...-squashfs-factory.bin` | hidden `fwupdate.html` page |

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

## From stock — PROVEN, no UART, no case opening

**This works.** Done 2026-09-11 on a second, never-opened SPNMX57 running stock
`MX57CF` firmware `1.0.1.216553`. The vendor updater accepts our unsigned
`factory.bin` as-is. No Linksys header, no signature, no serial console, no
disassembly.

The way in is a **hidden page Linksys document themselves**, in support article
7162, "How to manually update the firmware on a node using a hidden link". It is
not reachable from the normal admin navigation:

```
https://<router-ip>/fwupdate.html
```

It is plain HTTP Basic auth and a file field, which is why it is invisible to
any amount of JNAP API probing. It is not a JNAP action, and it is not one of
the `*.cgi` paths.

### Procedure

1. **Power cycle the router.** Not a reboot, pull the mains lead. The stock
   firmware wedges its userspace within a few minutes of every boot (see below),
   so you want a freshly booted unit and you want to move quickly.
2. Cable the router straight to your machine, **LAN port to your NIC**, nothing
   else attached. **Confirm the link negotiates at 1000baseT** before going
   further.
3. Browse to `https://<router-ip>/fwupdate.html`. Accept the self-signed
   certificate warning.
4. Enter the admin password. On a factory-fresh unit that is `admin`; if you
   have run the setup wizard it is whatever you set there.
5. Choose `...-squashfs-factory.bin`. **Not** `sysupgrade.bin` — the names differ
   by one word and they live in the same directory.
6. Click **Update**. A successful upload returns exactly:

   ```json
   { "result": "OK" }
   ```

7. Leave it alone. It writes and reboots on its own.

### What to expect afterwards

| | |
|---|---|
| Reboot | link drops for roughly 13 seconds |
| Address | **192.168.1.1** — OpenWrt's default, not the vendor's |
| Access | SSH on 22, no root password set |
| `board_name` | `linksys,spnmx57` |
| Ports | `lan1 lan2 lan3 wan`, all four present as DSA netdevs |
| Radios | `phy0` and `phy1` both detected, Wi-Fi disabled as OpenWrt ships it |
| `boot_part` | ended at **1**, so **stock survives in slot 2** |

That last row is the important one: this does not consume your way back. The
vendor firmware is still in the other slot and `auto_recovery=yes` is intact.

### The one real hazard: the stock firmware wedges

Repeatably, on every boot: the unit serves its pages for a few minutes, then
**every userspace service dies while the kernel stays up**. It pings with no
loss, ARP resolves, the link holds, and ports 80, 443 and 52000 all stop
answering. It does not recover on its own.

Two consequences:

- **The reset button stops working too**, because it is read by a userspace
  daemon. You can hold it for a minute and nothing happens. Only pulling the
  power recovers it. If a reset "does not take", check whether the box answers
  TCP at all before assuming you pressed the wrong button.
- **Watch link state, not the LED**, to confirm a reboot really happened. A
  power cycle drops the link; a button press on a wedged unit does not.

So: power cycle, then do the upload promptly. Do not spend the healthy window
exploring the UI.

### Why the web UI itself is useless here

Worth knowing so you do not waste time on it. The Smart Wi-Fi UI gates itself:

```js
IsAdminPasswordDefault -> false ? "configured"
                       -> true  ? ask nodes/setup/IsAdminPasswordSetByUser
```

"Configured" means only *the admin password is not the generic default*. An ISP
unit ships with a unique per-device password on its label, so a factory-fresh
unit is classed as configured and redirected to an app-download page, and the
"Continue to Linksys Smart Wi-Fi" link is inside a `configured-content` section
that is hidden on an unconfigured unit. Either way you do not reach a firmware
page. `fwupdate.html` bypasses all of it.

Also note `*.cgi` paths return `401` or `403` regardless of whether they exist —
verify with a made-up filename before reading anything into such a result. A
`.html` path does return an honest `404`, which is how `fwupdate.html` was
confirmed.

### U-Boot TFTP, if you ever do need it

Needs UART, and on the one unit opened so far the console was **read only** (the
adapter's TX never reached the router's RX), which is not enough to interrupt
autoboot. `bootdelay=3`, serial console only, no network console and no
button-triggered recovery. Prefer `fwupdate.html`.

```
setenv ipaddr <a free address on your LAN>
setenv serverip <your TFTP server>
tftp 0x44000000 factory.bin
```

Use `setenv`, never `saveenv`, so nothing persists if it goes wrong.

### A trap when testing on a directly-attached machine

A reset router comes up on `192.168.1.1`, and so does OpenWrt after this flash.
If your machine has a VPN carrying a subnet route for `192.168.1.0/24`, that
route can win and your probes will quietly reach a different site entirely.
Check with `route get 192.168.1.1` before trusting any result. Check interface
priority too: an ethernet service ordered above Wi-Fi will take the default
route from a router that has no internet behind it.

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
