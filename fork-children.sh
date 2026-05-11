#!/usr/bin/env bash

set -euo pipefail

count="${1:-1}"
start_index="${2:-1}"
parent_vm="${PARENT_VM:-parent-vm}"
child_prefix="${CHILD_PREFIX:-child-vm}"

if ! [[ "$count" =~ ^[0-9]+$ ]] || [ "$count" -lt 1 ]; then
	echo "usage: $0 [count] [start_index]" >&2
	exit 1
fi

if ! [[ "$start_index" =~ ^[0-9]+$ ]] || [ "$start_index" -lt 1 ]; then
	echo "usage: $0 [count] [start_index]" >&2
	exit 1
fi

for ((i = start_index; i < start_index + count; i++)); do
	child_vm="${child_prefix}${i}"

	echo "Preparing tap for ${child_vm}"
	./addtap.sh

	sleep 1

	echo "Forking ${parent_vm} -> ${child_vm}"
	sudo ./lkvm fork -n "${parent_vm}" -d "${child_vm}"

	sleep 1
done
