#!/bin/sh
# M1 side check: tiny11 WinPE on stock EDK2, EL1, no PMU, no HyperWin patcher.
# WinPE's boot files live in a RAM disk, so this does not depend on a disk driver.
set -eu
cd "$(dirname "$0")/../.."
ISO="${HYPERWIN_ISO:-/path/to/windows11-arm64 r1.iso}"
EDK2="$(brew --prefix qemu)/share/qemu/edk2-aarch64-code.fd"
if [ ! -f "$ISO" ]; then echo "ISO not found: $ISO" >&2; exit 1; fi
if [ ! -f "$EDK2" ]; then echo "EDK2 firmware not found: $EDK2" >&2; exit 1; fi
mkdir -p build
if [ ! -f build/edk2-vars.fd ]; then
  dd if=/dev/zero of=build/edk2-vars.fd bs=1048576 count=64 status=none
fi
SERIAL=build/m1-winpe-serial.log
INTLOG=build/m1-winpe-int.log
: > "$SERIAL"
: > "$INTLOG"
exec qemu-system-aarch64 \
  -machine virt,gic-version=3,virtualization=off \
  -cpu max,pmu=off -smp 2 -m 4096 \
  -drive if=pflash,format=raw,readonly=on,file="$EDK2" \
  -drive if=pflash,format=raw,file=build/edk2-vars.fd \
  -device qemu-xhci,id=xhci \
  -drive if=none,id=cd,file="$ISO",format=raw,readonly=on,media=cdrom \
  -device usb-storage,bus=xhci.0,drive=cd,bootindex=0 \
  -device ramfb \
  -serial file:"$SERIAL" \
  -gdb tcp::1234 \
  -d int,guest_errors -D "$INTLOG" \
  -display none \
  -no-reboot
