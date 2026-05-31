#!/usr/bin/env bash
# vnetif — load the module and optionally configure the first interface.
#
# Usage: sudo ./scripts/load.sh [IP_ADDRESS]
#
# Copyright (C) 2026 Ridha MASTOURI <mastouri.rida@gmail.com>
# SPDX-License-Identifier: GPL-2.0-only

set -euo pipefail

MODULE_PATH="build/vnetif.ko"
IF0="vnet0"

if [[ $EUID -ne 0 ]]; then
    echo "error: must be run as root" >&2
    exit 1
fi

if [[ ! -f "$MODULE_PATH" ]]; then
    echo "error: $MODULE_PATH not found — run 'make' first" >&2
    exit 1
fi

insmod "$MODULE_PATH"
echo "vnetif: module loaded"

# Prevent NetworkManager from managing the virtual interface.
nmcli device set "$IF0" managed no 2>/dev/null || true

if [[ ${1-} ]]; then
    echo "$IF0 $1" > /proc/vnetif/setaddr
    echo "vnetif: $IF0 configured with $1/24"
fi

echo "vnetif: interfaces:"
cat /proc/vnetif/status