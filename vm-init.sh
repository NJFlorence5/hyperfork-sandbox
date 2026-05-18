#!/bin/bash
set -euo pipefail

IFACE="enp0s0"
BASE_NET="192.168.50"
GATEWAY="192.168.50.1"
DNS="8.8.8.8"

if [ "$#" -ne 1 ]; then
    echo "Usage:"
    echo "  $0 <host-octet>"
    echo
    echo "Examples:"
    echo "  $0 2    # parent  -> 192.168.50.2"
    echo "  $0 3    # child1  -> 192.168.50.3"
    echo "  $0 4    # child2  -> 192.168.50.4"
    exit 1
fi

HOST_OCTET="$1"

if ! [[ "$HOST_OCTET" =~ ^[0-9]+$ ]]; then
    echo "[-] Host octet must be a number."
    exit 1
fi

if [ "$HOST_OCTET" -lt 2 ] || [ "$HOST_OCTET" -gt 254 ]; then
    echo "[-] Host octet must be between 2 and 254."
    exit 1
fi

IP="$BASE_NET.$HOST_OCTET"

# Locally administered MAC.
# Example:
#   host octet 2 -> 02:15:15:15:15:02
#   host octet 3 -> 02:15:15:15:15:03
#   host octet 4 -> 02:15:15:15:15:04
MAC_LAST=$(printf "%02x" "$HOST_OCTET")
MAC="02:15:15:15:15:$MAC_LAST"

echo "[+] Bringing $IFACE down..."
ip link set "$IFACE" down || true

echo "[+] Setting unique MAC address: $MAC"
ip link set "$IFACE" address "$MAC"

echo "[+] Flushing old addresses from $IFACE..."
ip addr flush dev "$IFACE" || true

echo "[+] Bringing $IFACE up..."
ip link set "$IFACE" up
echo "[+] Assigning IP address $IP/24..."
# Use 'replace' so re-running this script doesn't crash on 'File exists'.
ip addr replace "$IP/24" dev "$IFACE"

echo "[+] Adding default route via $GATEWAY..."
ip route replace default via "$GATEWAY"

echo "[+] Setting DNS to $DNS..."
rm -f /etc/resolv.conf
echo "nameserver $DNS" > /etc/resolv.conf

echo "[+] Flushing stale ARP/neighbor cache..."
ip neigh flush all || true

echo "[+] VM network initialized."
echo
ip addr show "$IFACE"
echo
ip route
echo
cat /etc/resolv.conf
