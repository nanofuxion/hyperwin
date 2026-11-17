#!/bin/sh
# Builds build/hwpci/hwpci.sys. No WDK: clang + mingw-w64 DDK headers + llvm-dlltool + lld-link.
set -eu
cd "$(dirname "$0")/../../.."
OUT=build/hwpci
MINGW=/opt/homebrew/opt/mingw-w64/toolchain-x86_64/x86_64-w64-mingw32/include
DLLTOOL=${DLLTOOL:-/opt/homebrew/opt/llvm/bin/llvm-dlltool}
LLD=${LLD:-lld-link}
mkdir -p "$OUT"
clang --target=aarch64-w64-mingw32 -nostdinc \
  -isystem guest/drivers/hwpci/shim -isystem guest/drivers/shim \
  -isystem "$MINGW" -isystem "$MINGW/ddk" -isystem "$(clang -print-resource-dir)/include" \
  -Iinclude -ffreestanding -fno-stack-protector -fno-builtin -O2 -Wall -D_M_ARM \
  -c guest/drivers/hwpci/hwpci.c -o "$OUT/hwpci.o"
cat > "$OUT/ntoskrnl.def" <<'DEF'
LIBRARY ntoskrnl.exe
EXPORTS
ExAllocatePool2
ExFreePool
IoAttachDeviceToDeviceStack
IoBuildDeviceIoControlRequest
IoCreateDevice
IoDeleteDevice
IoDetachDevice
IoGetDmaAdapter
IofCallDriver
IofCompleteRequest
KeAcquireSpinLockRaiseToDpc
KeGetCurrentIrql
KeInitializeEvent
KeReleaseSpinLock
KeSetEvent
KeWaitForSingleObject
IoBuildSynchronousFsdRequest
MmMapIoSpace
IoSetDevicePropertyData
IoDisconnectInterruptEx
IoConnectInterruptEx
IoDisconnectInterrupt
IoConnectInterrupt
ZwClose
PsTerminateSystemThread
PsCreateSystemThread
KeDelayExecutionThread
IoGetDevicePropertyData
MmUnmapIoSpace
ObfReferenceObject
PoCallDriver
PoSetPowerState
RtlCompareMemory
memcpy
memset
DEF
"$DLLTOOL" -m arm64 -d "$OUT/ntoskrnl.def" -l "$OUT/ntoskrnl.lib"
"$LLD" /driver /subsystem:native /entry:DriverEntry /machine:arm64 /nodefaultlib /release \
  /out:"$OUT/hwpci.sys" "$OUT/hwpci.o" "$OUT/ntoskrnl.lib"
osslsigncode sign -certs build/testcert/hyperwin-test.crt -key build/testcert/hyperwin-test.key -h sha256 \
  -n "HyperWin PCI" -in "$OUT/hwpci.sys" -out "$OUT/hwpci.signed.sys" >/dev/null
mv "$OUT/hwpci.signed.sys" "$OUT/hwpci.sys"
