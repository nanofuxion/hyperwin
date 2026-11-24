#!/bin/sh
# Builds build/files/hwfiles.exe (WinFsp service) and build/files/hwfiles.sys (kernel mapper).
# WinFsp itself (headers and the installer the guest gets) comes from build/third_party/winfsp.
set -eu
cd "$(dirname "$0")/../.."
WINFSP=build/third_party/winfsp
WINFSP_VERSION=2.1.25156
if [ ! -f "$WINFSP/winfsp-$WINFSP_VERSION.msi" ] || [ ! -d "$WINFSP/src/inc" ]; then
  mkdir -p "$WINFSP/src"
  curl -fsSL -o "$WINFSP/winfsp-$WINFSP_VERSION.msi" \
    "https://github.com/winfsp/winfsp/releases/download/v2.1/winfsp-$WINFSP_VERSION.msi"
  curl -fsSL "https://github.com/winfsp/winfsp/archive/refs/tags/v2.1.tar.gz" | tar xz -C "$WINFSP/src" --strip-components=1
fi
CC=build/third_party/win/llvm-mingw/bin/aarch64-w64-mingw32-clang
OUT=build/files
mkdir -p "$OUT"
"$CC" -O2 -Wall -Wextra -Wno-missing-field-initializers -Wno-unused-parameter -Wno-unknown-pragmas -Iinclude \
  -I"$WINFSP/src/inc" -o "$OUT/hwfiles.exe" guest/files/hwfiles.c -ladvapi32
osslsigncode sign -certs build/testcert/hyperwin-pkg-chain.crt -key build/testcert/hyperwin-pkg.key -h sha256 \
  -n "HyperWin files" -in "$OUT/hwfiles.exe" -out "$OUT/hwfiles.signed.exe" >/dev/null
mv "$OUT/hwfiles.signed.exe" "$OUT/hwfiles.exe"
cp "$WINFSP/winfsp-$WINFSP_VERSION.msi" "$OUT/winfsp.msi"

# Kernel mapper driver (hwfiles.sys, ARM64, ntoskrnl imports only), independent of hwgpu.sys.
MINGW=/opt/homebrew/opt/mingw-w64/toolchain-x86_64/x86_64-w64-mingw32/include
DLLTOOL=${DLLTOOL:-/opt/homebrew/opt/llvm/bin/llvm-dlltool}
clang --target=aarch64-w64-mingw32 -nostdinc -isystem guest/drivers/hwpci/shim -isystem guest/drivers/shim \
  -isystem "$MINGW" -isystem "$MINGW/ddk" -isystem "$(clang -print-resource-dir)/include" \
  -Iinclude -Igpu/include -ffreestanding -fno-stack-protector -fno-builtin -O2 -Wall -fms-extensions -D_M_ARM \
  -c guest/files/hwfiles_drv.c -o "$OUT/hwfiles_drv.o"
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
lld-link /driver /subsystem:native /entry:DriverEntry /machine:arm64 /nodefaultlib /release /tsaware:no \
  /out:"$OUT/hwfiles.unsigned.sys" "$OUT/hwfiles_drv.o" "$OUT/ntoskrnl.lib"
osslsigncode sign -certs build/testcert/hyperwin-test.crt -key build/testcert/hyperwin-test.key -h sha256 \
  -n "HyperWin phone storage driver" -in "$OUT/hwfiles.unsigned.sys" -out "$OUT/hwfiles.sys.new" >/dev/null
mv "$OUT/hwfiles.sys.new" "$OUT/hwfiles.sys"
echo "$OUT/hwfiles.exe $OUT/hwfiles.sys"
