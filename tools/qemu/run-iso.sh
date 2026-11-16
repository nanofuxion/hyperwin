#!/bin/sh
# Boot tiny11 ARM64 under EDK2. Does not wait for setup to finish.
set -eu
cd "$(dirname "$0")/../.."
ISO="${HYPERWIN_ISO:-/path/to/windows11-arm64 r1.iso}"
if [ ! -f "$ISO" ]; then
  echo "ISO not found: $ISO" >&2
  exit 1
fi
make test dll
mkdir -p build/oem/hyperwin
cp tools/unattend/autounattend.xml build/oem/autounattend.xml
echo hyperwin > build/oem/hyperwin.id
cp -R build/dist/. build/oem/hyperwin/
cp guest/icd/vulkan_gfxstream.json build/oem/hyperwin/
python3 tools/mkfat.py build/oem build/oem.img
EDK2="$(brew --prefix qemu)/share/qemu/edk2-aarch64-code.fd"
if [ ! -f build/edk2-vars.fd ]; then
  dd if=/dev/zero of=build/edk2-vars.fd bs=1048576 count=64 status=none
fi
if [ ! -f build/win.qcow2 ]; then
  qemu-img create -f qcow2 build/win.qcow2 40G
fi
./build/hwbridge init build/window.img
SERIAL=build/iso-serial.log
: > "$SERIAL"
# USB storage is what Windows PE can see without a virtio driver.
# virtio-gpu-pci is the same device AVF's gfxstream backend exposes.
exec qemu-system-aarch64 \
  -machine virt,virtualization=on,gic-version=3,nvdimm=on \
  -cpu max -smp 4 -m 4096,slots=2,maxmem=4352M \
  -drive if=pflash,format=raw,readonly=on,file="$EDK2" \
  -drive if=pflash,format=raw,file=build/edk2-vars.fd \
  -object memory-backend-file,id=winmem,size=128M,mem-path=build/window.img,share=on \
  -device nvdimm,memdev=winmem,id=nv0 \
  -device qemu-xhci,id=xhci \
  -drive if=none,id=cd,file="$ISO",format=raw,readonly=on,media=cdrom \
  -device usb-storage,bus=xhci.0,drive=cd,bootindex=0 \
  -drive if=none,id=disk,file=build/win.qcow2,format=qcow2 \
  -device usb-storage,bus=xhci.0,drive=disk \
  -drive if=none,id=oem,file=build/oem.img,format=raw \
  -device usb-storage,bus=xhci.0,drive=oem \
  -device virtio-gpu-pci \
  -device ramfb \
  -device usb-kbd,bus=xhci.0 \
  -device usb-tablet,bus=xhci.0 \
  -serial file:"$SERIAL" \
  -display none \
  -no-reboot
