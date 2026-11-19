#!/bin/bash
# Builds the Direct3D 10/11 user-mode driver (wddm/README.md phase 4) as ARM64X: hwd3d_umd.dll, which the
# adapter's INF names as its UserModeDriverName. The driver header comes from gpu/work/wdk (fetch-wdk.sh,
# never committed or shipped); gen_umd.py reads it into gpu/work/umd-gen. The DLL holds our code only.
#   gpu/guest/hwd3d/build-umd.sh      Output: gpu/work/umd-out/hwd3d_umd.dll
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$HERE/../../..
W=$(sh "$ROOT/gpu/guest/wddm/fetch-wdk.sh")
K=$W/microsoft.windows.wdk.arm64/c/Include/10.0.26100.0
S=$W/microsoft.windows.sdk.cpp/c/Include/10.0.26100.0
GEN=$ROOT/gpu/work/umd-gen
OUT=$ROOT/gpu/work/umd-out
INC=$ROOT/build/third_party/win/llvm-mingw/generic-w64-mingw32/include
mkdir -p "$OUT"
python3 "$HERE/gen_umd.py" "$K/um/d3d10umddi.h" "$GEN" "$K/um/d3dumddi.h"
python3 "$ROOT/gpu/host/hwd3d/gen_thunks.py" "$INC" "$ROOT/gpu/work/hwd3d-gen" >/dev/null
# mingw's headers first, the driver headers after: the driver header needs nothing mingw lacks but dxmini.h.
"$ROOT/gpu/guest/arm64x/clang++" -std=c++17 -O2 -Wall -Wno-unused-function -Wno-microsoft-anon-tag -Wno-missing-field-initializers \
  -Wno-ignored-attributes -D_WIN32_WINNT=0x0A00 -DUNICODE -I"$GEN" -I"$ROOT/gpu/work/hwd3d-gen" -I"$ROOT/include" -I"$ROOT/gpu/include" -I"$HERE" \
  -idirafter "$K/um" -idirafter "$K/shared" -idirafter "$S/um" -idirafter "$S/shared" \
  -shared -static -o "$OUT/hwd3d_umd.dll" "$HERE/umd.cpp" "$HERE/umd.def" -Wl,--entry,DllMain -lgdi32 -luser32
"$ROOT/build/third_party/win/llvm-mingw/bin/llvm-strip" "$OUT/hwd3d_umd.dll"
ls -la "$OUT"
# The 32-bit driver, for x86 programs under Windows' emulator (UserModeDriverNameWow). Plain i686, stdcall names undecorated.
W=$ROOT/gpu/work/umd-out
"$ROOT/build/third_party/win/llvm-mingw/bin/i686-w64-mingw32-clang++" -std=c++17 -O2 -Wall -Wno-unused-function -Wno-microsoft-anon-tag -Wno-missing-field-initializers \
  -Wno-ignored-attributes -D_WIN32_WINNT=0x0A00 -DUNICODE -I"$GEN" -I"$ROOT/gpu/work/hwd3d-gen" -I"$ROOT/include" -I"$ROOT/gpu/include" -I"$HERE" \
  -idirafter "$K/um" -idirafter "$K/shared" -idirafter "$S/um" -idirafter "$S/shared" \
  -shared -static -o "$W/hwd3d_umd_x86.dll" "$HERE/umd.cpp" "$HERE/umd.def" -Wl,--entry,DllMain@12 -Wl,--kill-at -lgdi32 -luser32
"$ROOT/build/third_party/win/llvm-mingw/bin/llvm-strip" "$W/hwd3d_umd_x86.dll"
ls -la "$W"
