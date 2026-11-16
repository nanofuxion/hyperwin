#!/bin/sh
# Direct Image boot, the same shape as SweetCider's setKernelPath payload.
# This QEMU build cannot hotplug an NVDIMM (it wants acpi-ged). The shared
# window is a 128 MiB slice of a file-backed RAM region at 16 MiB, which is
# the QEMU form of SweetCider's file-backed mapping.
set -eu
cd "$(dirname "$0")/../.."
make test payload
mkdir -p build
OFFSET=$((16 * 1024 * 1024))
SPAN=$((256 * 1024 * 1024))
./build/hwbridge init build/ram.img "$OFFSET" "$SPAN"
python3 -c 'import os; n=os.path.getsize("build/ram.img"); assert n%4096==0, n'
SERIAL=build/probe-serial.log
: > "$SERIAL"
./build/hwbridge run build/ram.img build/guest-frame.ppm 20 "$OFFSET" &
hpid=$!
qemu-system-aarch64 \
  -machine virt,virtualization=on,gic-version=3,memory-backend=ram0 \
  -cpu max -smp 2   -m 256M \
  -kernel build/hyperwin.img \
  -object memory-backend-file,id=ram0,size=256M,mem-path=build/ram.img,share=on \
  -device virtio-gpu-pci \
  -serial file:"$SERIAL" \
  -display none \
  -no-reboot &
qpid=$!
set +e
wait "$hpid"
rc=$?
set -e
kill "$qpid" 2>/dev/null || true
wait "$qpid" 2>/dev/null || true
echo "----- probe serial -----"
tr -cd '\11\12\15\40-\176' < "$SERIAL" | tail -n 40 || true
exit "$rc"
