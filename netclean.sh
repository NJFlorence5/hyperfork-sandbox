#!/bin/bash
set -euo pipefail

BRIDGE="br0"
TAP_PARENT="tap0"
TAP_CHILD="tap1"

echo "[*] Stopping old bridge/TAP state..."

sudo ip link set "$TAP_PARENT" down 2>/dev/null || true
sudo ip link set "$TAP_CHILD" down 2>/dev/null || true
sudo ip link set "$TAP_PARENT" nomaster 2>/dev/null || true
sudo ip link set "$TAP_CHILD" nomaster 2>/dev/null || true
sudo ip tuntap del dev "$TAP_PARENT" mode tap 2>/dev/null || true
sudo ip tuntap del dev "$TAP_CHILD" mode tap 2>/dev/null || true
sudo ip addr flush dev "$BRIDGE" 2>/dev/null || true
sudo ip link set "$BRIDGE" down 2>/dev/null || true
sudo ip link del "$BRIDGE" type bridge 2>/dev/null || true

echo "[+] Cleanup complete"
