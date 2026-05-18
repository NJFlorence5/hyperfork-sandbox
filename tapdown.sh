#!/bin/bash
exec > /tmp/tapdown.log 2>&1
echo "tapdown.sh called with args: $@"
TAP_DEV="$1"
/usr/sbin/ip link set "$TAP_DEV" down
/usr/sbin/ip link set "$TAP_DEV" nomaster
