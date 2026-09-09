#!/usr/bin/env python3
"""
ramboot-test.py - non-destructive bring-up loop for the Linksys SPNMX57.

Boots an OpenWrt initramfs image on the device straight from RAM via the vendor
U-Boot's TFTP, so nothing is written to NAND and a power-cycle always recovers.
This is the iteration harness for the new-stack (DSA) port.

What it does:
  1. reboots the box over the serial console
  2. aggressively interrupts the U-Boot autoboot (retries - the window is short)
  3. waits for the front-panel port link to settle (U-Boot 'ping' until alive;
     the PHY needs a few seconds after a reboot before TFTP will work)
  4. 'tftp <addr> <image>' then 'bootm <addr>'
  5. captures the boot and prints the lines you actually care about

Requires: pyserial, a USB-serial cable on the device UART, a TFTP server on this
host serving the image, and an ethernet cable from the host NIC to a device port.

Usage:
  ./ramboot-test.py                          # defaults below
  ./ramboot-test.py --image spnmx57.itb --serverip 192.168.1.10
  ./ramboot-test.py --grep 'eth0|qca8386|2500'
"""

import argparse
import re
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial not installed: pip3 install pyserial")

UBOOT_PROMPT = b"IPQ5018#"
DEFAULTS = {
    "dev": "/dev/cu.usbserial-110",
    "baud": 115200,
    "image": "spnmx57.itb",      # filename as served by the TFTP root
    "serverip": "192.168.1.10",  # this host, on the direct-cable NIC
    "loadaddr": "0x44000000",
    "grep": r"eth0|gmac|2500|EINVAL|qca8386|DSA|Link is Up|lan[1-4]|validation|cannot attach|phy",
}


def catch_uboot(p, attempts=3, window=45):
    """Reboot and interrupt autoboot. Returns True if we land at the prompt."""
    for attempt in range(1, attempts + 1):
        print(f"[*] reboot + catching U-Boot (attempt {attempt}/{attempts})...")
        p.reset_input_buffer()
        p.write(b"\r\nreboot\r\n")
        buf = b""
        end = time.time() + window
        while time.time() < end:
            p.write(b" ")               # any key stops autoboot
            buf += p.read(200)
            if UBOOT_PROMPT in buf[-40:]:
                print("[+] at U-Boot prompt")
                return True
            time.sleep(0.02)
        print("[-] missed the autoboot window (it booted through); retrying")
    return False


def wait_for_link(p, serverip, timeout=45):
    """U-Boot 'ping' until the peer answers - the PHY needs time after reboot."""
    print(f"[*] waiting for link / ping {serverip} ...")
    end = time.time() + timeout
    while time.time() < end:
        p.reset_input_buffer()
        p.write(f"ping {serverip}\r".encode())
        time.sleep(6)
        out = p.read(8000).decode(errors="replace")
        if "is alive" in out:
            speed = re.search(r"PORT\d+ Up Speed :(\d+)", out)
            print(f"[+] link up{' at ' + speed.group(1) + 'M' if speed else ''}, peer alive")
            return True
        time.sleep(1)
    print("[-] peer never answered - check the cable / host NIC IP / TFTP host")
    return False


def main():
    ap = argparse.ArgumentParser()
    for k, v in DEFAULTS.items():
        ap.add_argument(f"--{k}", default=v)
    ap.add_argument("--boot-wait", type=int, default=55,
                    help="seconds to capture after bootm")
    args = ap.parse_args()

    p = serial.Serial(args.dev, int(args.baud), timeout=0.2)

    if not catch_uboot(p):
        sys.exit("could not reach U-Boot; power-cycle and retry")

    p.reset_input_buffer()
    p.write(f"setenv serverip {args.serverip}\r".encode())
    time.sleep(0.6)
    p.read(2000)

    if not wait_for_link(p, args.serverip):
        sys.exit("no link to the TFTP host")

    print(f"[*] tftp {args.image} -> {args.loadaddr}")
    p.reset_input_buffer()
    p.write(f"tftp {args.loadaddr} {args.image}\r".encode())
    time.sleep(22)
    out = p.read(60000).decode(errors="replace")
    if "Bytes transferred" not in out:
        print(out[-800:])
        sys.exit("TFTP failed - is the image in the TFTP root and the server up?")
    size = re.search(r"Bytes transferred = (\d+)", out)
    print(f"[+] transferred {size.group(1) if size else '?'} bytes")

    print("[*] bootm - booting from RAM (nothing written to flash)")
    p.write(f"bootm {args.loadaddr}\r".encode())
    time.sleep(args.boot_wait)
    boot = p.read(200000).decode(errors="replace")
    p.close()

    print("\n=== matched lines ===")
    pat = re.compile(args.grep, re.I)
    for line in boot.splitlines():
        if pat.search(line):
            print("  " + line.strip())
    print("\n=== tail ===")
    print(boot[-1200:])


if __name__ == "__main__":
    main()
