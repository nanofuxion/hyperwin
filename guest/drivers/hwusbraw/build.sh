#!/bin/sh
# Builds and test-signs build/hwusbraw/hwusbraw.sys (ARM64, ntoskrnl imports only, no WDK file).
set -eu
cd "$(dirname "$0")/../../.."
OUT=build/hwusbraw
MINGW=/opt/homebrew/opt/mingw-w64/toolchain-x86_64/x86_64-w64-mingw32/include
DLLTOOL=${DLLTOOL:-/opt/homebrew/opt/llvm/bin/llvm-dlltool}
mkdir -p "$OUT"
clang --target=aarch64-w64-mingw32 -nostdinc -isystem guest/drivers/hwpci/shim -isystem guest/drivers/shim \
  -isystem "$MINGW" -isystem "$MINGW/ddk" -isystem "$(clang -print-resource-dir)/include" \
  -Iinclude -ffreestanding -fno-stack-protector -fno-builtin -O2 -Wall -fms-extensions -D_M_ARM \
  -c guest/drivers/hwusbraw/hwusbraw.c -o "$OUT/hwusbraw.o"
cat > "$OUT/ntoskrnl.def" <<'DEF'
LIBRARY ntoskrnl.exe
EXPORTS
IoCreateDevice
IoCreateSymbolicLink
IoDeleteDevice
IoDeleteSymbolicLink
IofCompleteRequest
MmMapIoSpace
MmUnmapIoSpace
RtlInitUnicodeString
SeSinglePrivilegeCheck
memcpy
memset
DEF
"$DLLTOOL" -m arm64 -d "$OUT/ntoskrnl.def" -l "$OUT/ntoskrnl.lib"
lld-link /driver /subsystem:native /entry:DriverEntry /machine:arm64 /nodefaultlib /release /tsaware:no \
  /out:"$OUT/hwusbraw.unsigned.sys" "$OUT/hwusbraw.o" "$OUT/ntoskrnl.lib"
osslsigncode sign -certs build/testcert/hyperwin-test.crt -key build/testcert/hyperwin-test.key -h sha256 \
  -n "HyperWin USB raw ring" -in "$OUT/hwusbraw.unsigned.sys" -out "$OUT/hwusbraw.sys.new" >/dev/null
mv "$OUT/hwusbraw.sys.new" "$OUT/hwusbraw.sys"
echo "$OUT/hwusbraw.sys"
