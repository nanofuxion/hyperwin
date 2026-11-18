#!/bin/sh
# Builds and test-signs build/hwhid/hwhid.sys (ARM64 touch screen and Precision Touchpad over vsock).
# The catalog is made inside Windows at install time (guest/drivers/install-driver.ps1).
set -eu
cd "$(dirname "$0")/../../.."
OUT=build/hwhid
MINGW=/opt/homebrew/opt/mingw-w64/toolchain-x86_64/x86_64-w64-mingw32/include
DLLTOOL=${DLLTOOL:-/opt/homebrew/opt/llvm/bin/llvm-dlltool}
mkdir -p "$OUT"
clang --target=aarch64-w64-mingw32 -nostdinc -isystem guest/drivers/hwpci/shim -isystem guest/drivers/shim \
  -isystem "$MINGW" -isystem "$MINGW/ddk" -isystem "$(clang -print-resource-dir)/include" \
  -Iinclude -ffreestanding -fno-stack-protector -fno-builtin -O2 -Wall -Wno-unknown-pragmas -D_M_ARM \
  -c guest/drivers/hwhid/hwhid.c -o "$OUT/hwhid.o"
cat > "$OUT/ntoskrnl.def" <<'DEF'
LIBRARY ntoskrnl.exe
EXPORTS
IofCallDriver
IofCompleteRequest
IoAcquireCancelSpinLock
IoReleaseCancelSpinLock
KeAcquireSpinLockRaiseToDpc
KeDelayExecutionThread
KeQueryPerformanceCounter
KeReleaseSpinLock
ObfReferenceObject
PoCallDriver
PoStartNextPowerIrp
PsCreateSystemThread
ZwClose
ZwCreateFile
ZwDeviceIoControlFile
ZwReadFile
memcpy
memmove
memset
wcslen
DEF
cat > "$OUT/hidclass.def" <<'DEF'
LIBRARY hidclass.sys
EXPORTS
HidRegisterMinidriver
DEF
"$DLLTOOL" -m arm64 -d "$OUT/ntoskrnl.def" -l "$OUT/ntoskrnl.lib"
"$DLLTOOL" -m arm64 -d "$OUT/hidclass.def" -l "$OUT/hidclass.lib"
# /tsaware:no: see guest/drivers/hwvsinput/build.sh.
lld-link /driver /subsystem:native /entry:DriverEntry /machine:arm64 /nodefaultlib /release /tsaware:no \
  /out:"$OUT/hwhid.unsigned.sys" "$OUT/hwhid.o" "$OUT/ntoskrnl.lib" "$OUT/hidclass.lib"
osslsigncode sign -certs build/testcert/hyperwin-test.crt -key build/testcert/hyperwin-test.key -h sha256 \
  -n "HyperWin touch" -in "$OUT/hwhid.unsigned.sys" -out "$OUT/hwhid.sys.new" >/dev/null
mv "$OUT/hwhid.sys.new" "$OUT/hwhid.sys"
# Windows keeps one driver package per INF text, so each driver build gets its own version.
H=$(shasum -a 256 "$OUT/hwhid.sys" | cut -c1-8)
sed "s/^DriverVer=.*/DriverVer=11\/18\/2026,1.0.$((0x$(echo $H | cut -c1-4))).$((0x$(echo $H | cut -c5-8)))/" \
  guest/drivers/hwhid/hwhid.inf > "$OUT/hwhid.inf"
echo "$OUT/hwhid.sys"
