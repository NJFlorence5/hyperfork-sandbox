#!/usr/bin/env bash
set -euo pipefail

VM_NAME="${1:-parent-vm}"

sudo ./lkvm run \
  --name "$VM_NAME" \
  --disk ./ubuntu-20-hyperfork.raw \
  --kernel "$HOME/linux-6.6.82/arch/x86/boot/bzImage" \
  --initrd "$HOME/hyperfork-kvmtool/initrd20.cpio" \
  --network mode=tap,tapif=tap0 \
  --console serial \
  -p "root=/dev/mapper/ubuntu--vg-ubuntu--lv rw console=ttyS0 init=/sbin/init"
