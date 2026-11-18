#!/bin/sh
# Builds build/hwgpuw/hwgpuw.sys (ARM64 WDDM render-only miniport, wddm/README.md) and signs it with the
# test certificate. The WDK headers come from gpu/work/wdk (fetch-wdk.sh); no WDK library is linked,
# DxgkInitialize is in hwgpuw.c, and the driver imports only ntoskrnl.
#   gpu/guest/wddm/build.sh
set -eu
cd "$(dirname "$0")/../../.."
W=$(sh gpu/guest/wddm/fetch-wdk.sh)
K=$W/microsoft.windows.wdk.arm64/c/Include/10.0.26100.0
S=$W/microsoft.windows.sdk.cpp/c/Include/10.0.26100.0
OUT=build/hwgpuw
DLLTOOL=${DLLTOOL:-/opt/homebrew/opt/llvm/bin/llvm-dlltool}
mkdir -p "$OUT"
clang --target=aarch64-pc-windows-msvc -fms-extensions -fms-compatibility -fms-compatibility-version=19.30 \
  -nostdinc -ffreestanding -fno-stack-protector -fno-builtin -O2 -Wall -Wno-unknown-pragmas -Wno-microsoft \
  -D_ARM64_ -DNTDDI_VERSION=0x0A000008 -D_WIN32_WINNT=0x0A00 -DWINNT=1 -DNT_INST=0 -DDXGKDDI_INTERFACE_VERSION=0x4002 \
  -isystem "$(clang -print-resource-dir)/include" -isystem "$K/km" -isystem "$K/shared" -isystem "$K/km/crt" -isystem "$S/shared" \
   -Iinclude -Igpu/include -c gpu/guest/wddm/hwgpuw.c -o "$OUT/hwgpuw.o"
cat > "$OUT/ntoskrnl.def" <<'DEF'
LIBRARY ntoskrnl.exe
EXPORTS
DEF
# The imports the object file asks for, taken from its own undefined symbols.
llvm-nm -u "$OUT/hwgpuw.o" | sed -n 's/^ *U *//p' | sed 's/^__imp_//' | sort -u | /usr/bin/grep -v '^$' | sed 's/^PsThreadType$/PsThreadType DATA/' >> "$OUT/ntoskrnl.def"
"$DLLTOOL" -m arm64 -d "$OUT/ntoskrnl.def" -l "$OUT/ntoskrnl.lib"
lld-link /driver /subsystem:native /entry:DriverEntry /machine:arm64 /nodefaultlib /release /tsaware:no \
  /out:"$OUT/hwgpuw.unsigned.sys" "$OUT/hwgpuw.o" "$OUT/ntoskrnl.lib"
osslsigncode sign -certs build/testcert/hyperwin-test.crt -key build/testcert/hyperwin-test.key -h sha256 \
  -n "HyperWin GPU" -in "$OUT/hwgpuw.unsigned.sys" -out "$OUT/hwgpuw.sys.new" >/dev/null
mv "$OUT/hwgpuw.sys.new" "$OUT/hwgpuw.sys"
echo "$OUT/hwgpuw.sys"
# The driver package: hwgpuw.sys, its INF (each build gets its own DriverVer, since Windows keeps one
# package per INF text) and the Vulkan driver that the INF registers on the adapter.
PKG=$OUT/pkg
rm -rf "$PKG"
mkdir -p "$PKG"
cp "$OUT/hwgpuw.sys" "$PKG/"
bash gpu/guest/hwd3d/build-umd.sh >/dev/null
cp gpu/work/umd-out/hwd3d_umd.dll "$PKG/"
H=$(cat "$OUT/hwgpuw.sys" "$PKG/hwd3d_umd.dll" | shasum -a 256 | cut -c1-8)
sed "s/^DriverVer *=.*/DriverVer   = 11\/18\/2026,1.0.$((0x$(echo $H | cut -c1-4))).$((0x$(echo $H | cut -c5-8)))/" \
  gpu/guest/wddm/hwgpuw.inf > "$PKG/hwgpuw.inf"
[ -f gpu/work/out/vulkan_hwgpu.dll ] && cp gpu/work/out/vulkan_hwgpu.dll gpu/work/out/vulkan_hwgpu.json "$PKG/"
echo "$PKG"
