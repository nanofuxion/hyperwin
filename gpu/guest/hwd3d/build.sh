#!/bin/bash
# Builds HwD3D's Windows DLLs (d3d/README.md) as ARM64X: d3d11.dll (the objects, the channel and the
# generated thunks) and dxgi.dll (hands over to it). Also checks every structure that crosses as
# bytes has the same size and alignment in Windows and in DXVK on the phone.
#   gpu/guest/hwd3d/build.sh      Output: gpu/work/out/dx/hwd3d/{d3d11,dxgi}.dll
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$HERE/../../..
INC=$ROOT/build/third_party/win/llvm-mingw/generic-w64-mingw32/include
GEN=$ROOT/gpu/work/hwd3d-gen
OUT=$ROOT/gpu/work/out/dx/hwd3d
CC=$ROOT/gpu/guest/arm64x/clang
STRIP=$ROOT/build/third_party/win/llvm-mingw/bin/llvm-strip
mkdir -p "$OUT"
python3 "$ROOT/gpu/host/hwd3d/gen_thunks.py" "$INC" "$GEN"

# Sizes: the generated list plus the structures the hand-written methods send whole.
DXN=$ROOT/gpu/work/dxvk-native/include
if [ -d "$DXN" ]; then
  P=$GEN/probe
  { echo '#include <windows.h>'; echo '#include <d3d11.h>'; echo '#include <dxgi.h>'
    for t in $(cat "$GEN/hwd3d_structs.txt") DXGI_SWAP_CHAIN_DESC D3D11_MAPPED_SUBRESOURCE; do
      echo "unsigned s_$t = sizeof($t) * 1000 + _Alignof($t);"
    done; } > "$P.c"
  "$ROOT/build/third_party/win/llvm-mingw/bin/aarch64-w64-mingw32-clang" -S -o "$P.win.s" "$P.c"
  NDK=${ANDROID_HOME:-$HOME/Library/Android/sdk}/ndk/27.2.12479018
  "$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android34-clang++" -w -x c++ -S -o "$P.phone.s" \
    -I"$DXN/native/windows" -I"$DXN/native/directx" -I"$DXN/native" "$P.c"
  for t in $(grep -o 's_[A-Za-z0-9_]*' "$P.c" | sort -u); do
    a=$(grep -A3 "^$t:" "$P.win.s" | grep -oE 'word[[:space:]]+[0-9]+' | head -1)
    b=$(grep -A3 "^$t:" "$P.phone.s" | grep -oE 'word[[:space:]]+[0-9]+' | head -1)
    [ "$a" = "$b" ] || { echo "${t#s_} differs: Windows $a, phone $b"; exit 1; }
  done
fi

"$CC" -O2 -Wall -Wno-unused-parameter -Wno-missing-field-initializers -I"$GEN" -I"$ROOT/include" -I"$ROOT/gpu/include" \
  -shared -o "$OUT/d3d11.dll" "$HERE/d3d11.c" "$HERE/d3d11.def"
"$CC" -O2 -Wall -shared -o "$OUT/dxgi.dll" "$HERE/dxgi.c"
"$STRIP" "$OUT/d3d11.dll" "$OUT/dxgi.dll"
# The phase-0 cube (gpu/host/hwd3d/cube.cpp) as a Windows test, ARM64 and x64.
T=$ROOT/build/third_party/win/llvm-mingw/bin
mkdir -p "$ROOT/gpu/work/out/dx/test"
for a in aarch64:arm64 x86_64:x64; do
  "$T/${a%%:*}-w64-mingw32-clang++" -std=c++17 -O2 -static -o "$ROOT/gpu/work/out/dx/test/d3dcube-${a##*:}.exe" \
    "$ROOT/gpu/host/hwd3d/cube.cpp" -ld3d11 -ldxgi
done
ls -la "$OUT"
