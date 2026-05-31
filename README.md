# vnetif — Virtual Network Interface Driver

A Linux kernel module that synthesizes virtual Ethernet interfaces
(`vnet0`, `vnet1`, …) controlled entirely through procfs.  Each interface
answers ARP requests and ICMP echo requests in-kernel — no userspace daemon,
no physical hardware required.  Interfaces are full `net_device` citizens:
they participate in routing, can join Linux bridges, and work across network
namespaces.

**Author:** Ridha MASTOURI \<mastouri.rida@gmail.com\>  
**License:** GPL-2.0-only  
**Version:** 1.0  
**Kernel:** ≥ 5.6

---

## Features

| Feature | Details |
|---|---|
| Virtual interfaces | `vnet0`, `vnet1`, … created on demand |
| ARP reply | Answers Ethernet/IPv4 ARP requests in-kernel |
| ICMP echo reply | Self-ping without a real network stack |
| Bridge support | Remote ping through a Linux bridge topology |
| ethtool | `ethtool -i/-S` for driver info and statistics |
| 64-bit statistics | `ndo_get_stats64` via `ip -s link show` |
| MTU control | 68–65535 bytes via procfs or `ip link set … mtu` |
| MAC address | Changeable via `ip link set … address` |
| Module parameter | `max_interfaces` configures the device ceiling |

---

## Requirements

| Dependency | Purpose |
|---|---|
| `linux-headers-$(uname -r)` | Kernel build environment |
| `gcc`, `make` | Module compilation |
| `cmake` ≥ 3.14 | Integration test build |
| `iproute2`, `bridge-utils` | Runtime and test networking |
| Root privileges | Module load, procfs writes, test execution |

---

## Project Structure

```
.
├── src/
│   ├── vnetif.h            shared types, constants, declarations
│   ├── vnetif_core.c       module lifecycle · device · address management
│   ├── vnetif_netdev.c     net_device ops · ARP/ICMP processing · ethtool
│   └── vnetif_proc.c       procfs control interface
├── tests/
│   ├── CMakeLists.txt      GoogleTest build
│   └── vnetif_test.cc      integration + negative-path test suite
├── scripts/
│   ├── load.sh             load module and optionally configure an IP
│   ├── unload.sh           cleanly unload the module
│   └── demo-bridge.sh      full bridge + namespace demo
├── Documentation/
│   └── vnetif.md           architecture and API reference
├── CHANGELOG.md
├── CONTRIBUTING.md
├── MAINTAINERS
└── Makefile
```

---

## Build

```bash
# Build the kernel module (produces build/vnetif.ko)
make

# Remove build artefacts
make clean
```

---

## Test

```bash
# Build + compile + run the full integration suite as root
make test
```

Test coverage includes: procfs permissions, interface lifecycle, IPv4
assignment, MTU change, MAC address change, error paths (invalid inputs,
nonexistent interfaces), local self-ping, and remote ping through a bridge.

> Run from the project root as **root**.

---

## Procfs Interface

```
/proc/vnetif/
├── status   (r--)  list interfaces, MTU, and IPv4 addresses
├── create   (-w-)  create a new interface (any write triggers it)
├── destroy  (-w-)  destroy an interface by name
├── setaddr  (-w-)  assign an IPv4 address: "IFNAME IP"
└── setmtu   (-w-)  change MTU: "IFNAME MTU"
```

---

## Module Parameters

| Parameter | Default | Description |
|---|---|---|
| `max_interfaces` | `16` | Maximum number of virtual interfaces |

```bash
sudo insmod build/vnetif.ko max_interfaces=32
```

---

## Quick Start

```bash
# Build and load
make
sudo scripts/load.sh 10.10.10.1

# Verify
cat /proc/vnetif/status
ethtool -i vnet0
ethtool -S vnet0

# Self-ping
ping -I vnet0 10.10.10.1 -c 3

# Change MTU to jumbo frame
echo 'vnet0 9000' | sudo tee /proc/vnetif/setmtu

# Add a second interface
echo 1 | sudo tee /proc/vnetif/create
echo 'vnet1 10.10.20.1' | sudo tee /proc/vnetif/setaddr

# Unload
sudo scripts/unload.sh
```

---

## Bridge Demo

```bash
sudo scripts/demo-bridge.sh
```

This script loads the module, builds a bridge + veth + namespace topology,
and pings `vnet0` from the isolated namespace to demonstrate the remote ping
path.  Press Enter to clean up everything automatically.

---

## Architecture

```
frame → ndo_start_xmit(vnetN)
           │
           ├─ unicast, not-self, bridge slave?
           │       └─ dev_queue_xmit(bridge master)          ← L2 forwarding
           │
           ├─ ARP REQUEST for our IP?
           │       └─ vnetif_pkt_build_arp_reply()
           │               └─ vnetif_pkt_deliver → netif_rx  ← reply injected
           │
           ├─ ICMP ECHO for our IP?
           │       └─ vnetif_pkt_build_icmp_reply()
           │               └─ vnetif_pkt_deliver → netif_rx  ← reply injected
           │
           └─ drop (tx_dropped++)
```

For full architectural detail see [`Documentation/vnetif.md`](Documentation/vnetif.md).

---

## ethtool

```bash
# Driver info
ethtool -i vnet0
# driver: vnetif  version: 2.0  bus-info: virtual

# Statistics
ethtool -S vnet0
# tx_packets: 12
# tx_bytes: 1440
# rx_packets: 12
# rx_bytes: 1440
# tx_dropped: 0
```

---

## License

Licensed under the **GNU General Public License v2.0 only** (`GPL-2.0-only`) —
the same license as the Linux kernel itself, which is required for modules
that use kernel-internal symbols.  See [LICENSE](LICENSE).