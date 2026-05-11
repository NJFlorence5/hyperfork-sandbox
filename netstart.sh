#!/bin/bash
set -euo pipefail

BRIDGE="br0"
HOST_IP="192.168.50.1/24"

echo "[*] Cleaning old TAP devices..."

for TAP in $(ip -o link show | awk -F': ' '{print $2}' | grep -E '^tap[0-9]+$' || true); do
    echo "    deleting $TAP"
    sudo ip link set "$TAP" down 2>/dev/null || true
    sudo ip link set "$TAP" nomaster 2>/dev/null || true
    sudo ip tuntap del dev "$TAP" mode tap 2>/dev/null || true
done

echo "[*] Cleaning old bridge $BRIDGE..."

sudo ip addr flush dev "$BRIDGE" 2>/dev/null || true
sudo ip link set "$BRIDGE" down 2>/dev/null || true
sudo ip link del "$BRIDGE" type bridge 2>/dev/null || true

echo "[*] Creating bridge $BRIDGE..."

sudo ip link add name "$BRIDGE" type bridge
sudo ip link set "$BRIDGE" type bridge stp_state 0 forward_delay 0
sudo ip addr add "$HOST_IP" dev "$BRIDGE"
sudo ip link set "$BRIDGE" up

echo "[+] Network start complete"
echo
ip addr show "$BRIDGE"
echo
bridge link
