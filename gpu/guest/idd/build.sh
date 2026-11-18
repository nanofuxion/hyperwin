#!/bin/bash
# Builds hwidd.dll, HyperWin's indirect display driver (UMDF 2 on IddCx, wddm/README.md phase 6), and its
# package in build/hwidd. The WDF headers are Microsoft's MIT-licensed Windows-Driver-Frameworks (fetched
# into gpu/work and patched for clang there); IddCx.h comes from the WDK in gpu/work/wdk (fetch-wdk.sh),
# headers only. No WDK library is linked: fxstub.cpp is the start-up the WDK stub would give.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
HERE=$ROOT/gpu/guest/idd
WORK=$ROOT/gpu/work
T=$ROOT/build/third_party/win/llvm-mingw/bin
OUT=$ROOT/build/hwidd
W=$(cd "$ROOT" && cd "$(sh gpu/guest/wddm/fetch-wdk.sh)" && pwd)
IDDV=${IDDV:-1.8}
IDD=$W/microsoft.windows.wdk.arm64/c/Include/10.0.26100.0/um/iddcx/$IDDV
[ -d "$WORK/Windows-Driver-Frameworks" ] || git clone -q --depth 1 https://github.com/microsoft/Windows-Driver-Frameworks.git "$WORK/Windows-Driver-Frameworks"
# UMDF 2.33's headers, with three spots MSVC accepts and clang does not.
rm -rf "$WORK/wdf-umdf"
cp -R "$WORK/Windows-Driver-Frameworks/src/publicinc/wdf/umdf/2.33" "$WORK/wdf-umdf"
( cd "$WORK/wdf-umdf"
  sed -i '' 's/&##_variable/\&_variable/' WudfWdm.h
  sed -i '' 's/^typedef enum _DEVICE_RELATION_TYPE      DEVICE_RELATION_TYPE, \*PDEVICE_RELATION_TYPE;/typedef int DEVICE_RELATION_TYPE, *PDEVICE_RELATION_TYPE;/' WudfWdm.h
  sed -i '' 's/^typedef enum _WDF_REQUEST_TYPE WDF_REQUEST_TYPE;/typedef int WDF_REQUEST_TYPE_FWD;/' wdfdevice.h
  sed -i '' 's/^    WDF_REQUEST_TYPE RequestType$/    WDF_REQUEST_TYPE_FWD RequestType/' wdfdevice.h )
mkdir -p "$OUT/pkg"
FLAGS=(-O2 -std=c++17 -fms-extensions -Wno-everything -include "$HERE/mingw-shim.h" -DUMDF_VERSION_MAJOR=2 -DUMDF_VERSION_MINOR=33
       -DUMDF_MINIMUM_VERSION_REQUIRED=25 -DIDDCX_VERSION_MAJOR=1 -DIDDCX_VERSION_MINOR=${IDDV#1.} -DIDDCX_MINIMUM_VERSION_REQUIRED=4
       -I"$WORK/wdf-umdf" -I"$IDD" -I"$HERE")
for f in fxstub hwidd; do "$T/aarch64-w64-mingw32-clang++" "${FLAGS[@]}" -c "$HERE/$f.cpp" -o "$OUT/$f.o"; done
"$T/aarch64-w64-mingw32-clang++" -shared -static -o "$OUT/pkg/hwidd.dll" "$OUT/fxstub.o" "$OUT/hwidd.o" -ld3d11 -ldxgi \
  -Wl,--exclude-all-symbols -Wl,--export-all-symbols:no 2>/dev/null || \
"$T/aarch64-w64-mingw32-clang++" -shared -static -o "$OUT/pkg/hwidd.dll" "$OUT/fxstub.o" "$OUT/hwidd.o" -ld3d11 -ldxgi
"$T/llvm-strip" "$OUT/pkg/hwidd.dll"
H=$(shasum -a 256 "$OUT/pkg/hwidd.dll" | cut -c1-8)
sed "s/^DriverVer *=.*/DriverVer   = 11\/18\/2026,1.0.$((0x$(echo $H | cut -c1-4))).$((0x$(echo $H | cut -c5-8)))/" "$HERE/hwidd.inf" > "$OUT/pkg/hwidd.inf"
echo "$OUT/pkg"
