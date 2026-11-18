#!/bin/sh
# Builds and test-signs gpu/work/direct/hwgpu.sys (ARM64, ntoskrnl imports only).
set -eu
cd "$(dirname "$0")/../../.."
OUT=gpu/work/direct
MINGW=/opt/homebrew/opt/mingw-w64/toolchain-x86_64/x86_64-w64-mingw32/include
DLLTOOL=${DLLTOOL:-/opt/homebrew/opt/llvm/bin/llvm-dlltool}
mkdir -p "$OUT"
clang --target=aarch64-w64-mingw32 -nostdinc -isystem guest/drivers/hwpci/shim -isystem guest/drivers/shim \
  -isystem "$MINGW" -isystem "$MINGW/ddk" -isystem "$(clang -print-resource-dir)/include" \
  -Iinclude -Igpu/include -ffreestanding -fno-stack-protector -fno-builtin -O2 -Wall -fms-extensions -D_M_ARM \
  -c gpu/guest/direct/hwgpu.c -o "$OUT/hwgpu.o"
cat > "$OUT/ntoskrnl.def" <<'DEF'
LIBRARY ntoskrnl.exe
EXPORTS
__C_specific_handler
ExAcquireFastMutex
ExAllocatePoolWithTag
ExFreePoolWithTag
ExReleaseFastMutex
IoAllocateMdl
IoCreateDevice
IoCreateSymbolicLink
IoDeleteDevice
IoDeleteSymbolicLink
IoFreeMdl
IofCompleteRequest
KeInitializeEvent
MmBuildMdlForNonPagedPool
MmMapIoSpace
MmMapLockedPagesSpecifyCache
MmUnmapIoSpace
MmUnmapLockedPages
IoGetCurrentProcess
RtlInitUnicodeString
SeSinglePrivilegeCheck
memcpy
memset
DEF
"$DLLTOOL" -m arm64 -d "$OUT/ntoskrnl.def" -l "$OUT/ntoskrnl.lib"
# /tsaware:no: see guest/drivers/hwvsinput/build.sh. This driver loads after boot.
lld-link /driver /subsystem:native /entry:DriverEntry /machine:arm64 /nodefaultlib /release /tsaware:no \
  /out:"$OUT/hwgpu.unsigned.sys" "$OUT/hwgpu.o" "$OUT/ntoskrnl.lib"
osslsigncode sign -certs build/testcert/hyperwin-test.crt -key build/testcert/hyperwin-test.key -h sha256 \
  -n "HyperWin GPU ring" -in "$OUT/hwgpu.unsigned.sys" -out "$OUT/hwgpu.sys.new" >/dev/null
mv "$OUT/hwgpu.sys.new" "$OUT/hwgpu.sys"
echo "$OUT/hwgpu.sys"
