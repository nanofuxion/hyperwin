#!/bin/bash
# dxtest (Direct3D 11 and 12) and gltest (OpenGL) for ARM64 and x64: gpu/work/out/dx/test/
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../../.." && pwd)
T=$ROOT/build/third_party/win/llvm-mingw/bin
OUT=$ROOT/gpu/work/out/dx/test
mkdir -p "$OUT"
for a in aarch64:arm64 x86_64:x64; do
  "$T/${a%%:*}-w64-mingw32-clang++" -O1 -static -o "$OUT/dxtest-${a##*:}.exe" "$HERE/dxtest.cpp" -ld3d11 -ld3d12 -ldxgi
  "$T/${a%%:*}-w64-mingw32-clang" -O1 -o "$OUT/gltest-${a##*:}.exe" "$HERE/gltest.c" -lopengl32 -lgdi32
done
ls -la "$OUT"
