#!/usr/bin/env bash
set -euo pipefail

VM_NAME="${1:-parent-vm}"
VM_FILES_DIR="$(pwd)/vm_files"

sudo ./lkvm run \
  --name "$VM_NAME" \
  --disk "$VM_FILES_DIR/cow_mount/ubuntu-20-hyperfork.raw,clone" \
  --kernel "$VM_FILES_DIR/hyperfork-kernel-6.6.82/bzImage" \
  --initrd "$VM_FILES_DIR/initrd20.cpio" \
  --network mode=tap,script=$(pwd)/tapup.sh,downscript=$(pwd)/tapdown.sh,guest_mac=02:15:15:15:15:02 \
  --console serial \
  -p "root=/dev/mapper/ubuntu--vg-ubuntu--lv rw console=ttyS0 init=/sbin/init clocksource=tsc tsc=reliable no-kvmclock"
