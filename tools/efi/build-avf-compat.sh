#!/bin/sh
# Build only the AVF compatibility ESP; leave firmware and Windows media alone.
set -eu
cd "$(dirname "$0")/../.."
out=build/avf-esp
/opt/homebrew/opt/llvm/bin/clang --target=aarch64-unknown-elf -c guest/efi/el0_pmccntr.S -o "$out/el0.o"
/opt/homebrew/opt/llvm/bin/llvm-objcopy -O binary --only-section=.text "$out/el0.o" "$out/el0.bin"
python3 tools/efi/embed_aml.py "$out/el0_pmccntr.h" HyperwinEl0Pmccntr="$out/el0.bin"
/opt/homebrew/opt/llvm/bin/clang --target=aarch64-unknown-elf -c guest/efi/el1_pmu_trap.S -o "$out/el1.o"
/opt/homebrew/opt/llvm/bin/llvm-objcopy -O binary --only-section=.text "$out/el1.o" "$out/el1.bin"
python3 tools/efi/embed_aml.py "$out/el1_pmu_trap.h" HyperwinEl1PmuTrap="$out/el1.bin"
CC="/opt/homebrew/opt/llvm/bin/clang --target=aarch64-pc-windows-msvc -ffreestanding -fno-stack-protector -fshort-wchar -mno-red-zone -fno-builtin -O2 -Iinclude -Icore -I$out -DHYPERWIN_FREESTANDING"
LINK=/opt/homebrew/opt/lld/bin/lld-link
$CC -DHYPERWIN_STANDALONE_COMPAT -DHYPERWIN_MARKS_IN_WINDOW -c guest/efi/trace.c -o "$out/compat.obj"
$LINK -subsystem:efi_boot_service_driver -entry:StandaloneCompatEntry -out:"$out/COMPAT.EFI" "$out/compat.obj"
# Loader (first boot file) and display driver. qemu_ring.c stands in for the parts COMPAT.EFI carries.
$CC -c guest/efi/setup_boot.c -o "$out/setup_boot.obj"
$CC -c guest/efi/pci_acpi.c -o "$out/pci_acpi.obj"
$LINK -subsystem:efi_application -entry:SetupBootEntry -out:"$out/BOOTAA64.EFI" "$out/setup_boot.obj" "$out/pci_acpi.obj"
$CC -c guest/efi/gop_driver.c -o "$out/gop.obj"
$CC -c guest/efi/qemu_ring.c -o "$out/qemu_ring.obj"
$LINK -subsystem:efi_boot_service_driver -entry:GopDriverEntry -out:"$out/GOP.EFI" "$out/gop.obj" "$out/qemu_ring.obj"
python3 tools/mkfat12.py "$out/setup-esp.img" --file "$out/BOOTAA64.EFI" EFI/BOOT/BOOTAA64.EFI --file "$out/GOP.EFI" EFI/BOOT/GOP.EFI --file "$out/COMPAT.EFI" EFI/BOOT/COMPAT.EFI
cp "$out/setup-esp.img" app/src/main/assets/setup-esp.img
