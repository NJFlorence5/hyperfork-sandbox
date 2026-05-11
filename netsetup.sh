#!/bin/bash
set -euo pipefail

BRIDGE="br0"
TAP_PARENT="tap0"
TAP_CHILD="tap1"
HOST_IP="192.168.50.1/24"

echo "[*] Cleaning old network state..."
sudo ip link set "$TAP_PARENT" down 2>/dev/null || true
sudo ip link set "$TAP_CHILD" down 2>/dev/null || true
sudo ip link set "$TAP_PARENT" nomaster 2>/dev/null || true
sudo ip link set "$TAP_CHILD" nomaster 2>/dev/null || true
sudo ip tuntap del dev "$TAP_PARENT" mode tap 2>/dev/null || true
sudo ip tuntap del dev "$TAP_CHILD" mode tap 2>/dev/null || true
sudo ip addr flush dev "$BRIDGE" 2>/dev/null || true
sudo ip link set "$BRIDGE" down 2>/dev/null || true
sudo ip link del "$BRIDGE" type bridge 2>/dev/null || true

echo "[*] Creating bridge $BRIDGE..."
sudo ip link add name "$BRIDGE" type bridge
sudo ip link set "$BRIDGE" up
sudo ip addr add "$HOST_IP" dev "$BRIDGE"

echo "[*] Creating TAP devices $TAP_PARENT and $TAP_CHILD..."
sudo ip tuntap add dev "$TAP_PARENT" mode tap
sudo ip tuntap add dev "$TAP_CHILD" mode tap

echo "[*] Attaching TAP devices to bridge..."
sudo ip link set "$TAP_PARENT" master "$BRIDGE"
sudo ip link set "$TAP_CHILD" master "$BRIDGE"

echo "[*] Bringing TAP devices up..."
sudo ip link set "$TAP_PARENT" up
sudo ip link set "$TAP_CHILD" up

echo
echo "[+] Setup complete"
echo
ip a show "$BRIDGE"
ip a show "$TAP_PARENT"
ip a show "$TAP_CHILD"
bridge link
