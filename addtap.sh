#!/bin/bash
set -euo pipefail

BRIDGE="br0"
MAX_TAPS=1024

if ! ip link show "$BRIDGE" >/dev/null 2>&1; then
    echo "[-] Bridge $BRIDGE does not exist. Run ./netstart.sh first." >&2
    exit 1
fi

for i in $(seq 0 "$MAX_TAPS"); do
    TAP="tap$i"

    if ! ip link show "$TAP" >/dev/null 2>&1; then
        echo "[*] Creating $TAP..." >&2

        sudo ip tuntap add dev "$TAP" mode tap
        sudo ip link set "$TAP" master "$BRIDGE"
        sudo ip link set "$TAP" up

        echo "[+] Created $TAP on $BRIDGE" >&2

        # Clean output for scripting
        echo "$TAP"
        exit 0
    fi
done

echo "[-] No available TAP name found from tap0 to tap$MAX_TAPS" >&2
exit 1
