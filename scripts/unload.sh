#!/usr/bin/env bash
# vnetif — unload the module and remove all vnetN interfaces.
#
# Copyright (C) 2026 Ridha MASTOURI <mastouri.rida@gmail.com>
# SPDX-License-Identifier: GPL-2.0-only

set -euo pipefail

if [[ $EUID -ne 0 ]]; then
    echo "error: must be run as root" >&2
    exit 1
fi

if ! lsmod | grep -q "^vnetif "; then
    echo "vnetif: module is not loaded"
    exit 0
fi

rmmod vnetif
echo "vnetif: module unloaded"