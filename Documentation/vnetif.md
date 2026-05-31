# vnetif — Technical Reference

**Author:** Ridha MASTOURI \<mastouri.rida@gmail.com\>  
**Version:** 2.0  
**License:** GPL-2.0-only  
**Kernel:** ≥ 5.6 (uses `proc_ops`)

---

## Overview

`vnetif` is a Linux kernel module that synthesizes virtual Ethernet interfaces
(`vnet0`, `vnet1`, …).  Each interface answers ARP and ICMP echo requests
entirely in-kernel — no userspace daemon, no hardware.  Interfaces are
standard `net_device` instances and participate normally in routing, bridging,
and network namespaces.

---

## Architecture

### Source layout

```
src/
├── vnetif.h          shared types, constants, cross-file declarations
├── vnetif_core.c     module lifecycle · device management · address config
├── vnetif_netdev.c   net_device ops · ARP/ICMP processing · ethtool
└── vnetif_proc.c     procfs control interface
```

### Data model

```
vnetif_devlist  (LIST_HEAD, protected by vnetif_devlock mutex)
│
└── vnetif_entry ──► net_device
                         └── vnetif_priv (netdev_priv)
                                 ├── id      sequential creation index
                                 └── ipv4    configured IPv4 address (be32)
```

### Transmit path (`ndo_start_xmit`)

```
frame arrives on vnetN xmit queue
        │
        ├─ unicast, not self, bridge slave? ──► dev_queue_xmit(bridge master)
        │
        ├─ ARP REQUEST for our IPv4?
        │       └── vnetif_pkt_build_arp_reply()
        │               └── vnetif_pkt_deliver()  [netif_rx]
        │
        ├─ ICMP ECHO REQUEST for our IPv4?
        │       └── vnetif_pkt_build_icmp_reply()
        │               └── vnetif_pkt_deliver()  [netif_rx]
        │
        └─ drop (dev_kfree_skb, tx_dropped++)
```

### Self-ping vs. remote ping

| Scenario | Destination MAC | Delivery mechanism |
|---|---|---|
| Self-ping | `dev->dev_addr` (our own) | `pkt_type = PACKET_HOST`, `netif_rx` |
| Remote (bridge) | requester MAC | `netif_rx`, bridge learns MAC in FDB |

For remote ping, the bridge's LOCAL FDB entry for `vnet0`'s MAC must be
removed so the bridge forwards frames to `vnet0`'s xmit handler rather than
delivering them locally (see `scripts/demo-bridge.sh`).

---

## Procfs interface

The module creates `/proc/vnetif/` on load and removes it on unload.

### `/proc/vnetif/status` (mode `0444`)

Lists all interfaces, their MTU, and IPv4 address.

```
$ cat /proc/vnetif/status
vnet0            mtu=1500   10.10.10.1
vnet1            mtu=9000   -
```

Format: `IFNAME  mtu=MTU  IP|"-"` one line per interface.

### `/proc/vnetif/create` (mode `0222`)

Any write creates a new interface (`vnet1`, `vnet2`, …).

```bash
echo 1 | sudo tee /proc/vnetif/create
```

Returns `ENOSPC` when the `max_interfaces` limit is reached.

### `/proc/vnetif/destroy` (mode `0222`)

Write an interface name to remove it.

```bash
echo vnet1 | sudo tee /proc/vnetif/destroy
```

Returns `ENOENT` if the interface does not exist.

### `/proc/vnetif/setaddr` (mode `0222`)

Write `"IFNAME IP"` to assign an IPv4 address.

```bash
echo 'vnet0 10.10.10.1' | sudo tee /proc/vnetif/setaddr
```

The address is applied via `ip addr replace … /24` and persisted in the
device's `vnetif_priv` so ARP and ICMP replies use it.  Returns `ENOENT` for
unknown interfaces and `EINVAL` for malformed IP strings.

### `/proc/vnetif/setmtu` (mode `0222`)

Write `"IFNAME MTU"` to change the interface MTU (68–65535).

```bash
echo 'vnet0 9000' | sudo tee /proc/vnetif/setmtu
```

Calls `dev_set_mtu()` which notifies the kernel stack of the change.

---

## Module parameters

| Parameter | Type | Default | Description |
|---|---|---|---|
| `max_interfaces` | `uint` | `16` | Maximum number of virtual interfaces |

```bash
sudo insmod build/vnetif.ko max_interfaces=32
```

---

## ethtool integration

```bash
# Driver information
ethtool -i vnet0

# Per-interface statistics
ethtool -S vnet0
```

Available statistics: `tx_packets`, `tx_bytes`, `rx_packets`, `rx_bytes`,
`tx_dropped`.

---

## Locking

| Lock | Protects | Notes |
|---|---|---|
| `vnetif_devlock` (mutex) | `vnetif_devlist`, `vnetif_next_id`, `vnetif_priv.ipv4` | Never held across `rtnl_lock` |
| `rtnl_lock` (kernel) | `dev_open`, `dev_set_mtu` | Acquired/released within helpers |

`vnetif_dev_set_mtu` performs `dev_hold` / `dev_put` across the lock boundary
to prevent destruction of a device while `dev_set_mtu` is in progress.

---

## Known limitations

- IPv4 only: no IPv6 / NDP support
- Single address per interface (last `setaddr` wins)
- Subnet mask hardcoded to `/24` for the usermode `ip addr replace` call
  (the kernel-side ARP/ICMP matching uses the exact address, not the prefix)
- No hardware offload, TSO, or checksum offload