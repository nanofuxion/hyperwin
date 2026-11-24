#!/bin/sh
# Builds and test-signs build/hwcpufreq/hwcpufreq.sys (experimental demand-start CPPC frequency bridge).
set -eu
cd "$(dirname "$0")/../../.."
OUT=build/hwcpufreq
MINGW=/opt/homebrew/opt/mingw-w64/toolchain-x86_64/x86_64-w64-mingw32/include
DLLTOOL=${DLLTOOL:-/opt/homebrew/opt/llvm/bin/llvm-dlltool}
mkdir -p "$OUT"
clang --target=aarch64-w64-mingw32 -nostdinc -isystem guest/drivers/hwpci/shim -isystem guest/drivers/shim \
  -isystem "$MINGW" -isystem "$MINGW/ddk" -isystem "$(clang -print-resource-dir)/include" \
  -Iinclude -ffreestanding -fno-stack-protector -fno-builtin -O2 -Wall -D_M_ARM \
  -c guest/drivers/hwcpufreq/hwcpufreq.c -o "$OUT/hwcpufreq.o"
cat > "$OUT/ntoskrnl.def" <<'DEF'
LIBRARY ntoskrnl.exe
EXPORTS
KeInitializeEvent
KeSetEvent
KeWaitForSingleObject
ZwQuerySystemInformation
KeSetSystemAffinityThreadEx
KeRevertToUserAffinityThreadEx
KeQueryActiveProcessorCountEx
MmMapIoSpace
MmUnmapIoSpace
PsCreateSystemThread
PsTerminateSystemThread
ZwWaitForSingleObject
ZwClose
memset
memcpy
DEF
"$DLLTOOL" -m arm64 -d "$OUT/ntoskrnl.def" -l "$OUT/ntoskrnl.lib"
# /tsaware:no: lld sets the "terminal server aware" bit by default, and Windows then refuses to load
# the driver after boot (STATUS_CONFLICTING_ADDRESSES, device problem 39). Only the boot loader
# ignores it. Windows demotes this driver to demand start (StartOverride), so it must load late.
lld-link /driver /subsystem:native /entry:DriverEntry /machine:arm64 /nodefaultlib /release /tsaware:no \
  /out:"$OUT/hwcpufreq.unsigned.sys" "$OUT/hwcpufreq.o" "$OUT/ntoskrnl.lib"
osslsigncode sign -certs build/testcert/hyperwin-test.crt -key build/testcert/hyperwin-test.key -h sha256 \
  -n "HyperWin experimental CPU frequency bridge" -in "$OUT/hwcpufreq.unsigned.sys" -out "$OUT/hwcpufreq.sys.new" >/dev/null
mv "$OUT/hwcpufreq.sys.new" "$OUT/hwcpufreq.sys"
echo "$OUT/hwcpufreq.sys"
