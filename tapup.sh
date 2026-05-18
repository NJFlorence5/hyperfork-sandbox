#!/bin/bash
exec > /tmp/tapup.log 2>&1
echo "tapup.sh called with args: $@"
TAP_DEV="$1"
/usr/sbin/ip link set "$TAP_DEV" master br0
/usr/sbin/ip link set "$TAP_DEV" up
echo "ip link master result: $?"
