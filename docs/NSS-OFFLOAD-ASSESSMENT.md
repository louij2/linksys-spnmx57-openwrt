# NSS offload on the SPNMX57 — assessment (2026-09-09)

Prompted by **LS3434** on the forum reporting an SPNMX56 with working NSS on
kernel 6.18 + the upstream DWMAC stack, and asking whether the SPNMX57 could get
the same.

Source: [IPQ5018: NSS offload on kernel 6.18 with the upstream ethernet stack
(GL-B3000)](https://forum.openwrt.org/t/ipq5018-nss-offload-on-kernel-6-18-with-the-upstream-ethernet-stack-gl-b3000/253014)
by kuncy7.

- OpenWrt tree: `https://github.com/kuncy7/openwrt-nss-edma/tree/ipq50xx-nss`
- NSS packages: `https://github.com/kuncy7/nss-packages/tree/ipq50xx-nss`

## What it is

It is **not** a return to qca-nss-dp / qca-ssdk. It keeps the upstream
`stmmac`/`dwmac-ipq5018` driver and adds a small `qca-dwmac-nss` module that
claims the datapath through a new stmmac API — it redirects `ndo_start_xmit`
while phylink, MDIO and the netdev stay with the host. The offload engine is the
IPQ5018's **UBI32 core** running proprietary NSS firmware.

## Their measured numbers (GL-B3000, single 1 GbE CPU port)

| | result |
|---|---|
| routed NAT | ~66k pps one way, ~100k pps the other, **CPU 93-96% idle** |
| TCP ceiling | ~911 Mbit/s (their single 1G CPU port is the limit, not the SoC) |
| UDP one-way | 900 Mbit/s, 0.45% loss |
| **Wi-Fi** | **734/447 Mbit/s single stream, 791/534 with three, CPU ~90% idle** |
| worst idle seen | 68% |

For comparison, ours on the DSA stack: **385/512 Mbit/s with the CPU saturated.**

## The catch, and it is a big one

**DSA user ports and NSS/ECM acceleration are mutually exclusive on this SoC.**
Their design requires `unbind`ing the switch driver after boot and reprogramming
the switch fabric directly over MDIO from a dedicated module. They ship
`qca8337-nss` for the QCA8337; `rtl8367-nss` is in progress for the RTL8367S.

So adopting this means **giving up `lan1`/`lan2`/`lan3`/`wan` as separate
netdevs** — the exact feature that motivated this port. You would be back to a
single interface with VLAN separation programmed directly into the switch,
architecturally much like the old qca-ssdk stack, but far faster.

## What it would take for us

We would need to write a **`qca8386-nss`** module: program the QCA8386 fabric
directly over MDIO, no DSA.

We are unusually well placed to do that, because we already have the parts that
cost the most to work out:

- the correct 32-bit MDIO register decode (`off & 0x1f`, high half at
  `reg | BIT(1)`, window `(off>>5)&0x7`, page `(off>>8)&0xffff`) — the bug that
  ate days
- the switch-core register map (isisc family: PORT_STATUS 0x7c, PORT_HDR_CTL
  0x9c, FORWARD_CTL0/1 0x620/0x624, PORT_LOOKUP_CTL 0x660)
- the full bring-up sequence: work mode, switch-core memory config, UNIPHY1
  SerDes, nsscc clocks and resets
- port membership / VLAN programming, already working in `qca8386.c`

Effectively `qca8386-nss` is our existing `qca8386_setup()` and register
accessors, minus the DSA glue, plus whatever interface `qca-dwmac-nss` expects.

## Differences from their validated setup

| | theirs (GL-B3000) | ours (SPNMX57) |
|---|---|---|
| switch | QCA8337 | **QCA8386** — module does not exist |
| 5 GHz radio | QCN6122 | **QCN9074** — wifi offload may differ |
| CPU ports | one 1 GbE | 2.5 G conduit |
| CMN PLL fix | required | already have it (upstream `86b584bd0994`) |

Our 2.5 G conduit is interesting: their TCP ceiling of ~911 Mbit/s is their
single 1 G port, not the SoC. We might exceed it.

## Known issues they document

- NSS core is not reset on a soft reboot, leaving stale firmware; needs the
  core-local reset clamp asserted before the copy
- three silent ordering failures: arming on a down interface, mid-netifd
  takeover, VLAN manager timing
- Wi-Fi offload initially looked broken; that was a missing `gcc_ubi0_core_clk`

## The decision

This is a genuine fork, not a free upgrade:

- **Stay on DSA:** per-port netdevs, standard OpenWrt config, trivial dumb-AP
  mode, ~385/512 with a saturated CPU. What v0.5.0 ships.
- **Go NSS:** ~900 Mbit/s routed and ~734/447 Wi-Fi at ~90% CPU idle, but lose
  per-port netdevs and take on writing and maintaining `qca8386-nss` plus a
  proprietary firmware blob.

Worth noting for the extender use case specifically: NSS helps **Wi-Fi** too
(734/447 vs our 385/512), so the gain is not limited to routing.

**Not started.** Recorded so the decision is made deliberately rather than by
drift.
