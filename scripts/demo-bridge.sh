#!/usr/bin/env bash
# vnetif — bridge + network namespace demo.
#
# Creates a topology where a remote namespace can ping vnet0 through a bridge:
#
#   [ns1: veth-ns 10.10.10.2/24] <---> [veth-host] <---> [br-demo] <---> [vnet0 10.10.10.1]
#
# Usage: sudo ./scripts/demo-bridge.sh
#
# Copyright (C) 2026 Ridha MASTOURI <mastouri.rida@gmail.com>
# SPDX-License-Identifier: GPL-2.0-only

set -euo pipefail

MODULE_PATH="build/vnetif.ko"
BR="br-demo"
VETH_HOST="veth-host"
VETH_NS="veth-ns"
NS="vnetif-demo"
VNET_IP="10.10.10.1"
NS_IP="10.10.10.2/24"

cleanup() {
    ip netns del "$NS"   2>/dev/null || true
    ip link del "$VETH_HOST" 2>/dev/null || true
    ip link del "$BR"        2>/dev/null || true
    rmmod vnetif             2>/dev/null || true
    echo "demo: cleaned up"
}
trap cleanup EXIT

if [[ $EUID -ne 0 ]]; then
    echo "error: must be run as root" >&2
    exit 1
fi

echo "demo: loading vnetif..."
insmod "$MODULE_PATH"
nmcli device set vnet0 managed no 2>/dev/null || true
echo "$VNET_INTERFACE $VNET_IP" > /proc/vnetif/setaddr || true
echo "vnet0 $VNET_IP" > /proc/vnetif/setaddr

echo "demo: creating bridge $BR..."
ip link add "$BR" type bridge
ip link set "$BR" address 00:00:00:00:00:01
ip link set "$BR" up

echo "demo: creating veth pair..."
ip link add "$VETH_HOST" type veth peer name "$VETH_NS"
ip link set "$VETH_HOST" master "$BR"
ip link set "$VETH_HOST" up

echo "demo: attaching vnet0 to bridge..."
ip link set vnet0 master "$BR"
ip link set vnet0 up

VNET0_MAC=$(ip link show dev vnet0 | awk '/link\/ether/{print $2}')
bridge fdb del "$VNET0_MAC" dev vnet0 master 2>/dev/null || true

echo "demo: creating namespace $NS..."
ip netns add "$NS"
ip link set "$VETH_NS" netns "$NS"
ip netns exec "$NS" ip link set lo up
ip netns exec "$NS" ip link set "$VETH_NS" up
ip netns exec "$NS" ip addr add "$NS_IP" dev "$VETH_NS"

echo "demo: topology ready. Pinging $VNET_IP from namespace $NS..."
echo ""
ip netns exec "$NS" ping "$VNET_IP" -c 4

echo ""
echo "demo: press Enter to clean up..."
read -r