#!/bin/sh
# Ground truth from a running SPNMX57 on OpenWrt. READ-ONLY: writes nothing,
# changes no config. Run over Wi-Fi (Ethernet is the broken part) and redirect:
#
#   ssh root@<router> 'sh -s' < scripts/collect.sh > collected/$(date +%F).txt
#
# The prize is /sys/firmware/fdt: the exact device tree the kernel booted with.
# Decompile it on your machine with:  dtc -I dtb -O dts -o running.dts running.dtb
sec() { echo; echo "===== $* ====="; }

sec identity
cat /tmp/sysinfo/model 2>/dev/null
cat /tmp/sysinfo/board_name 2>/dev/null
. /etc/openwrt_release 2>/dev/null && echo "$DISTRIB_ID $DISTRIB_RELEASE $DISTRIB_REVISION $DISTRIB_TARGET"
uname -a
cat /proc/cmdline

sec "the ethernet failure"
dmesg | grep -iE "qca|mdio|phy|eth|dsa|switch|stmmac|edma" || echo "nothing matched"

sec "full boot log"
dmesg

sec "MDIO bus -- what is ACTUALLY on it"
ls -l /sys/bus/mdio_bus/devices/ 2>/dev/null || echo "no mdio_bus"
for d in /sys/bus/mdio_bus/devices/*/; do
  [ -d "$d" ] || continue
  printf '%s\n' "$d"
  for f in phy_id phy_interface uevent; do
    [ -f "$d$f" ] && printf '   %-14s %s\n' "$f" "$(tr "\n" " " < "$d$f")"
  done
done

sec "WIFI -- is the internal 2.4GHz radio up, or only the PCIe one?"
# The vendor enables BOTH: wifi@c000000 (qcom,ipq5018-wifi, internal) and
# wifi3@f00000 (qcom,cnss-qcn9000, the QCN9074 on PCIe). "Wi-Fi works" has
# never been pinned to one or both. Two phys here means both came up.
iw dev 2>/dev/null || echo "no iw"
echo "--- phy count: $(ls -d /sys/class/ieee80211/phy* 2>/dev/null | wc -l) ---"
for p in /sys/class/ieee80211/phy*; do
  [ -d "$p" ] || continue
  echo "$p  name=$(cat $p/name 2>/dev/null)  addr=$(cat $p/macaddress 2>/dev/null)"
  echo "   device: $(readlink -f $p/device 2>/dev/null)"
done

sec "ath11k -- which firmware and which board file"
# If no Linksys-SPNMX57 calibration variant exists upstream, ath11k falls back
# to a generic board file SILENTLY and the radios get wrong regulatory/power.
dmesg | grep -iE "ath11k|qmi|board-2|bdf|board_id|fallback" || echo "nothing matched"
ls -l /lib/firmware/ath11k/ 2>/dev/null
find /lib/firmware/ath11k -name "board*" 2>/dev/null

sec "PCIe -- which controller enumerated"
lspci 2>/dev/null || echo "no lspci"
ls -l /sys/bus/pci/devices/ 2>/dev/null

sec "LEDs and buttons"
ls -l /sys/class/leds/ 2>/dev/null || echo "no leds"
cat /sys/kernel/debug/gpio 2>/dev/null | head -40 || echo "no gpio debugfs"

sec "MAC addresses -- label MACs or random?"
for n in /sys/class/net/*/address; do
  [ -f "$n" ] && printf '%-28s %s\n' "$n" "$(cat $n)"
done
cat /sys/bus/nvmem/devices/*/nvmem 2>/dev/null | strings 2>/dev/null | grep -iE "mac|hw_" | head -20

sec "thermal"
for z in /sys/class/thermal/thermal_zone*; do
  [ -d "$z" ] && echo "$z $(cat $z/type 2>/dev/null) $(cat $z/temp 2>/dev/null)"
done

sec "network devices"
ip -br link 2>/dev/null || ifconfig -a
ls -l /sys/class/net/ 2>/dev/null

sec "DSA / switch"
ls -l /sys/class/net/*/dsa 2>/dev/null
swconfig list 2>/dev/null || echo "no swconfig (expected on DSA)"

sec "device tree the kernel booted with"
if [ -f /sys/firmware/fdt ]; then
  echo "/sys/firmware/fdt present, $(wc -c < /sys/firmware/fdt) bytes"
  echo "--- base64 below: decode to running.dtb and decompile with dtc ---"
  # NB: this box has neither base64 nor uuencode. Pull it directly instead:
  #   ssh -T root@<host> 'cat /sys/firmware/fdt' > running.dtb
  base64 /sys/firmware/fdt 2>/dev/null \
    || od -An -tx1 -v /sys/firmware/fdt 2>/dev/null \
    || echo "NO ENCODER: fetch /sys/firmware/fdt directly over ssh"
else
  echo "ABSENT -- fall back to /proc/device-tree"
  find /proc/device-tree -maxdepth 3 2>/dev/null | sort
fi

sec "flash layout and recovery options"
cat /proc/mtd 2>/dev/null
cat /proc/partitions 2>/dev/null
ubinfo -a 2>/dev/null | head -40
fw_printenv 2>/dev/null | grep -iE "boot|part|image" || echo "no fw_printenv (u-boot env not exposed)"

sec "loaded modules"
lsmod 2>/dev/null | sort
