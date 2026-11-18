#!/bin/bash
# Builds DXVK for Android (d3d/README.md, HwD3D) and the phase-0 cube test into gpu/work/hwd3d:
#   libdxvk_d3d11.so libdxvk_dxgi.so libdxvk_d3d10core.so libdxvk_d3d9.so libdxvk_d3d8.so hwd3d_cube
# DXVK's native build with dxvk-android.patch: an Android WSI (a window is an ANativeWindow) and
# Vulkan from the driver wrapper named by HWGPU_VK_LOADER. Its own copy of the source, apart from
# gpu/work/dxvk, which the Windows build (gpu/guest/dx/build.sh) uses unchanged.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=$HERE/../../work
DXVK_TAG=v3.1.1
SRC=$WORK/dxvk-native
BUILD=$WORK/dxvk-android
OUT=$WORK/hwd3d
NDK=${ANDROID_HOME:-$HOME/Library/Android/sdk}/ndk/27.2.12479018
CXX=$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android34-clang++

if [ ! -d "$SRC" ]; then
  git clone --branch "$DXVK_TAG" --depth 1 --recurse-submodules --shallow-submodules https://github.com/doitsujin/dxvk.git "$SRC"
  git -C "$SRC" apply "$HERE/dxvk-android.patch"
fi
[ -d "$BUILD" ] || meson setup "$BUILD" "$SRC" --cross-file "$WORK/gfx-android.ini" --buildtype release \
  -Dnative_sdl2=disabled -Dnative_sdl3=disabled -Dnative_glfw=disabled
ninja -C "$BUILD"
mkdir -p "$OUT"
for p in d3d11 dxgi d3d10 d3d9 d3d8; do cp "$BUILD"/src/$p/libdxvk_*.so "$OUT/"; done
"$CXX" -std=c++17 -O2 -fPIE -pie -Wall -Wno-extern-c-compat \
  -I"$SRC/include/native/windows" -I"$SRC/include/native/directx" -I"$SRC/include/native" \
  -I"$SRC/include/vulkan/include" "$HERE/cube.cpp" -L"$OUT" -ldxvk_d3d11 -ldxvk_dxgi -lmediandk -landroid \
  -o "$OUT/hwd3d_cube"
ls "$OUT"
