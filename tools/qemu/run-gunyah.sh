#!/bin/sh
# QEMU diagnostic approximation of non-protected AVF. QEMU virt uses ECAM,
# PL011 and PL031; build the HYPERWIN_QEMU firmware variant first.
set -eu
cd "$(dirname "$0")/../.."
ISO=${HYPERWIN_ISO:-/path/to/windows11-arm64 r1.iso}
FW=${HYPERWIN_FIRMWARE:-firmware/edk2-gunyah/edk2-gunyah.fd}
ESP=${HYPERWIN_ESP:-build/qemu-esp/setup-esp.img}
[ -f "$ISO" ] && [ -f "$FW" ] || { echo 'Missing ISO or firmware' >&2; exit 1; }
mkdir -p build
exec qemu-system-aarch64 \
  -machine virt,gic-version=3,virtualization=off -cpu max,pmu=off -smp 1 -m 4096 \
  -kernel "$FW" \
  -drive "if=none,id=esp,file=$ESP,format=raw,readonly=on" \
  -device virtio-blk-pci,drive=esp,bootindex=0 \
  -drive "if=none,id=install,file=$ISO,format=raw,readonly=on" \
  -device virtio-blk-pci,drive=install,bootindex=1 \
  -device virtio-gpu-pci -device virtio-keyboard-pci -device virtio-mouse-pci \
  -serial file:build/gunyah-qemu-debug-serial.log \
  -monitor unix:build/gunyah-monitor.sock,server=on,wait=off \
  -display "${HYPERWIN_DISPLAY:-none}" -no-reboot
