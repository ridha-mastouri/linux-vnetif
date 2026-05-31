# Changelog

All notable changes to vnetif are documented here.  
Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [1.0] — 2026-05-31

First public release.

### Features

- **Virtual Ethernet interfaces** — `vnet0`, `vnet1`, … created and removed at
  runtime; backed by a clean `net_device` implementation
- **In-kernel ARP reply** — answers Ethernet/IPv4 ARP requests (RFC 826)
  without a userspace daemon
- **In-kernel ICMP echo reply** — responds to IPv4 ping (RFC 792); supports
  local self-ping and remote ping through a Linux bridge
- **Bridge slave support** — standard L2 forwarding when the interface is
  attached to a Linux bridge
- **ethtool integration** — `ethtool -i` driver info, `ethtool -S` statistics
  (`tx_packets`, `tx_bytes`, `rx_packets`, `rx_bytes`, `tx_dropped`)
- **64-bit statistics** via `ndo_get_stats64` (`ip -s link show`)
- **MTU control** — 68–65535 bytes via `/proc/vnetif/setmtu` or
  `ip link set … mtu`
- **MAC address change** via `ip link set … address`
- **`max_interfaces` module parameter** — configures the device ceiling at
  load time (default: 16)

### Procfs interface

```
/proc/vnetif/
├── status   (r--)  list interfaces, MTU, and IPv4 addresses
├── create   (-w-)  create a new interface
├── destroy  (-w-)  destroy an interface by name
├── setaddr  (-w-)  assign an IPv4 address: "IFNAME IP"
└── setmtu   (-w-)  change MTU: "IFNAME MTU"
```

### Project structure

```
src/
├── vnetif.h          shared types, constants, declarations
├── vnetif_core.c     module lifecycle · device · address management
├── vnetif_netdev.c   net_device ops · ARP/ICMP · ethtool
└── vnetif_proc.c     procfs control interface
tests/                GoogleTest integration suite (32 test cases)
scripts/              load.sh · unload.sh · demo-bridge.sh
Documentation/        vnetif.md — architecture and API reference
```