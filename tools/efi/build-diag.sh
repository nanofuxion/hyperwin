#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
mkdir -p build/firmware-diag
/opt/homebrew/opt/llvm/bin/clang --target=aarch64-pc-windows-msvc -ffreestanding -fno-stack-protector -fno-builtin -O2 -Wall -Wextra -Werror -c guest/efi/firmware_diag.c -o build/firmware-diag/firmware_diag.obj
/opt/homebrew/opt/lld/bin/lld-link -machine:arm64 -subsystem:efi_application -entry:EfiMain -nodefaultlib -out:build/firmware-diag/DIAGAA64.EFI build/firmware-diag/firmware_diag.obj
