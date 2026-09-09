# Two-unit lab PoC (no ISP router involved)

Purpose: prove the port on a second unit and get the throughput numbers that a
single-unit bench cannot produce, on an isolated network. Afterwards both units
go into the home network or the Italy house, so the lab deliberately exercises
the two roles they will actually be used in: **router** and **dumb AP /
extender**.

Nothing here touches the Technicolor. There is no internet in the lab, which is
fine — every test below is local.

## Topology

```
  SPNMX57 #2  "upstream"            plays the part of the ISP box
    br-lan 192.168.2.1/24, DHCP
    lan1 ─────────────────────┐
                              │
  SPNMX57 #1  "device under test"
    wan  ◄────────────────────┘     takes a DHCP lease from #2
    br-lan 192.168.1.1/24, DHCP
    ├─ lan3 ── Mac                  (2.5G USB NIC)
    ├─ lan1 ── 2.5GbE unmanaged switch ── host B (second 2.5G endpoint)
    └─ lan2 ── spare
```

**Unit 1 needs no configuration change.** It is already `wan: dhcp` with
`br-lan 192.168.1.1` and `lan1 lan2 lan3` bridged. You physically move its wan
cable from the home LAN to unit 2's lan1 and it just works.

## What each test actually proves

| test | path taken | proves |
|---|---|---|
| Mac ↔ host B | lan3 → lan1, **inside the switch fabric** | hardware 2.5G switching, CPU not involved. **The number we have never had.** |
| Mac → 192.168.2.1 | lan3 → CPU → NAT → wan | routed/NAT throughput, CPU-bound |
| Mac → unit 1 itself | lan3 → CPU | CPU-terminated (the ~293 Mbit/s figure) |
| unplug/replug lan1, lan2 | — | link events and STP on ports that have **never had a partner** |
| unit 2 reconfigured as dumb AP | — | the extender role, which is the end state |

The first row is the whole point. Until bridge offload was implemented
(commit `c52d6b3`) that path went through the CPU and would have measured ~300
Mbit/s while looking like a hardware ceiling.

## Configs

### Unit 2, lab role: "upstream"

```sh
uci set network.lan.ipaddr='192.168.2.1'
uci -q delete network.wan
uci -q delete network.wan6
uci commit network
/etc/init.d/network restart
```

That is all. It keeps its own DHCP server on 192.168.2.0/24 and hands unit 1 a
lease. Its wan port is left unused.

### Unit 2, end-state role: dumb AP / extender

For the home network or Italy, after the PoC. Assumes the main router is
`192.168.1.1` and hands out DHCP.

```sh
# all four sockets become LAN
uci set network.@device[0].ports='lan1 lan2 lan3 wan'
# no routing, no NAT, just a bridge with a management address
uci set network.lan.proto='static'
uci set network.lan.ipaddr='192.168.1.2'
uci set network.lan.netmask='255.255.255.0'
uci set network.lan.gateway='192.168.1.1'
uci set network.lan.dns='192.168.1.1'
uci -q delete network.wan
uci -q delete network.wan6
# do not run a second DHCP server on the same subnet
uci set dhcp.lan.ignore='1'
uci commit
/etc/init.d/network restart
```

Note: pulling `wan` into `br-lan` is only valid in AP mode. In router mode it
must stay out, and the driver enforces that correctly — verified, wan's switch
member mask reads CPU-only while lan1-3 carry each other.

## Test commands

Run `iperf3 -s` on host B, then from the Mac:

```sh
# hardware switching, lan3 <-> lan1, should NOT load unit 1's CPU
iperf3 -c <host B> -t 30
iperf3 -c <host B> -t 30 -R
iperf3 -c <host B> -t 30 -P 4        # parallel streams

# watch unit 1's CPU while the above runs - it should stay near idle.
# if it does not, traffic is going through the CPU and offload is not working
ssh root@192.168.1.1 'top -b -n2 -d5 | grep -E "^CPU|idle"'

# NAT / routed path for comparison (expect far lower, CPU-bound)
iperf3 -c 192.168.2.1 -t 30
```

## Blockers before this can be built

1. **Unit 2 is on stock firmware and flashing it is an untested path.** Unit 1
   reached OpenWrt via `sysupgrade` from an already-OpenWrt state. Unit 2 needs
   `factory.bin` through the OEM web UI or TFTP recovery, which **nobody has done
   on this device**. `docs/flashing.md` is explicit that factory.bin must never
   go through sysupgrade. **Attach a UART to unit 2 before starting** — this is
   the one step in the project that can actually lose hardware.
2. **Going isolated costs the current TFTP path.** RAM-boot testing currently
   TFTPs over unit 1's wan to `arm` (10.0.0.249) on the home LAN. Once wan moves
   to unit 2 that route is gone. The Mac is on the lab network and could serve
   TFTP, but **macOS tftpd is currently down** and `en18` picks the wrong source
   address toward the box. Both need sudo. Fix that first or development drops
   to sysupgrade-only, which writes NAND every iteration.
3. **A second 2.5G endpoint is required** for the headline test. The Mac is one.
   The SPNMX57 is a poor endpoint (dual A53, ~293 Mbit/s) and would become the
   bottleneck, measuring nothing useful. Without a second 2.5G host we can still
   show the CPU stays idle during LAN-to-LAN traffic, which proves offload works,
   but not the line rate.

## Order of work

1. Fix the Mac's tftpd and `en18` addressing (sudo).
2. Attach UART to unit 2, flash it, confirm it boots. Riskiest step.
3. Apply the "upstream" config to unit 2.
4. Recable per the topology.
5. Run the tests, record real numbers.
6. Reconfigure unit 2 to the dumb-AP role and integrate both into the target
   network.
