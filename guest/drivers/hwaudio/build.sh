#!/bin/sh
# Builds and test-signs build/hwaudio/hwaudio.sys (ARM64 WaveRT speakers and microphone over the
# shared window). The catalog is made inside Windows at install time (guest/drivers/hwaudio/install.ps1).
set -eu
cd "$(dirname "$0")/../../.."
OUT=build/hwaudio
MINGW=/opt/homebrew/opt/mingw-w64/toolchain-x86_64/x86_64-w64-mingw32/include
DLLTOOL=${DLLTOOL:-/opt/homebrew/opt/llvm/bin/llvm-dlltool}
mkdir -p "$OUT"
clang --target=aarch64-w64-mingw32 -nostdinc -isystem guest/drivers/hwpci/shim -isystem guest/drivers/shim \
  -isystem "$MINGW" -isystem "$MINGW/ddk" -isystem "$(clang -print-resource-dir)/include" \
  -Iinclude -ffreestanding -fno-stack-protector -fno-builtin -O2 -Wall -Wno-unknown-pragmas -Wno-missing-braces -D_M_ARM \
  -c guest/drivers/hwaudio/hwaudio.c -o "$OUT/hwaudio.o"
cat > "$OUT/ntoskrnl.def" <<'DEF'
LIBRARY ntoskrnl.exe
EXPORTS
ExAllocatePoolWithTag
ExAllocateTimer
ExCancelTimer
ExDeleteTimer
ExFreePoolWithTag
ExSetTimer
IoAllocateMdl
IoFreeMdl
KeAcquireSpinLockRaiseToDpc
KeReleaseSpinLock
KeSetEvent
MmBuildMdlForNonPagedPool
MmMapIoSpace
memset
DEF
cat > "$OUT/portcls.def" <<'DEF'
LIBRARY portcls.sys
EXPORTS
PcAddAdapterDevice
PcInitializeAdapterDriver
PcNewPort
PcRegisterPhysicalConnection
PcRegisterSubdevice
DEF
"$DLLTOOL" -m arm64 -d "$OUT/ntoskrnl.def" -l "$OUT/ntoskrnl.lib"
"$DLLTOOL" -m arm64 -d "$OUT/portcls.def" -l "$OUT/portcls.lib"
# /tsaware:no: see guest/drivers/hwvsinput/build.sh.
lld-link /driver /subsystem:native /entry:DriverEntry /machine:arm64 /nodefaultlib /release /tsaware:no \
  /out:"$OUT/hwaudio.unsigned.sys" "$OUT/hwaudio.o" "$OUT/ntoskrnl.lib" "$OUT/portcls.lib"
osslsigncode sign -certs build/testcert/hyperwin-test.crt -key build/testcert/hyperwin-test.key -h sha256 \
  -n "HyperWin audio" -in "$OUT/hwaudio.unsigned.sys" -out "$OUT/hwaudio.sys.new" >/dev/null
mv "$OUT/hwaudio.sys.new" "$OUT/hwaudio.sys"
# Windows keeps one driver package per INF text, so each driver build gets its own version.
H=$(shasum -a 256 "$OUT/hwaudio.sys" | cut -c1-8)
sed "s/^DriverVer=.*/DriverVer=11\/17\/2026,1.0.$((0x$(echo $H | cut -c1-4))).$((0x$(echo $H | cut -c5-8)))/" \
  guest/drivers/hwaudio/hwaudio.inf > "$OUT/hwaudio.inf"
echo "$OUT/hwaudio.sys"
