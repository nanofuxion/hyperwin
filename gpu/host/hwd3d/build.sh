#!/bin/bash
# Builds DXVK for Android (d3d/README.md, HwD3D) and the phase-0 cube test into gpu/work/hwd3d:
#   libdxvk_d3d11.so libdxvk_dxgi.so libdxvk_d3d10core.so libdxvk_d3d9.so libdxvk_d3d8.so hwd3d_cube
# DXVK's native build with dxvk-android.patch: an Android WSI (a window is an ANativeWindow) and
# Vulkan from the driver wrapper named by HWGPU_VK_LOADER. Its own copy of the source, apart from
# gpu/work/dxvk, which the Windows build (gpu/guest/dx/build.sh) uses unchanged.
#
# Another version, as a zip for the app's Direct3D setting (d3d/README.md, "Choosing the DXVK version"):
#   gpu/host/hwd3d/build.sh zip <tag-or-branch> [repository url]
# builds that DXVK (2.1 or newer, from doitsujin/dxvk or a fork) with the same patch, into
# gpu/work/hwd3d/dxvk-<tag>.zip: meta.json and libdxvk_*.so. The patch is written against 3.1.1 and may
# need small fixes for versions far from it.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=$HERE/../../work
DXVK_TAG=v3.1.1
DXVK_URL=https://github.com/doitsujin/dxvk.git
ZIP=
if [ "${1:-}" = zip ]; then
  ZIP=1
  DXVK_TAG=${2:?usage: build.sh zip <tag-or-branch> [repository url]}
  DXVK_URL=${3:-$DXVK_URL}
  SAFE=$(echo "$DXVK_TAG" | tr -c 'A-Za-z0-9.\n' '-')
  SRC=$WORK/dxvk-src-$SAFE
  BUILD=$WORK/dxvk-build-$SAFE
else
  SRC=$WORK/dxvk-native
  BUILD=$WORK/dxvk-android
fi
OUT=$WORK/hwd3d
NDK=${ANDROID_HOME:-$HOME/Library/Android/sdk}/ndk/27.2.12479018
CXX=$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android34-clang++

if [ ! -d "$SRC" ]; then
  git clone --branch "$DXVK_TAG" --depth 1 --recurse-submodules --shallow-submodules "$DXVK_URL" "$SRC"
  git -C "$SRC" apply "$HERE/dxvk-android.patch" || git -C "$SRC" apply --3way --ignore-whitespace "$HERE/dxvk-android.patch" || {
    echo "dxvk-android.patch does not apply to $DXVK_TAG; fix it in $SRC and run again" >&2
    exit 1
  }
fi
# 16 KB pages: Android 15+ warns at install about libraries aligned for 4 KB.
PAGES=-Wl,-z,max-page-size=16384
[ -d "$BUILD" ] || meson setup "$BUILD" "$SRC" --cross-file "$WORK/gfx-android.ini" --buildtype release \
  -Dnative_sdl2=disabled -Dnative_sdl3=disabled -Dnative_glfw=disabled
meson configure "$BUILD" -Dc_link_args="$PAGES" -Dcpp_link_args="$PAGES"
ninja -C "$BUILD"
mkdir -p "$OUT"
if [ -n "$ZIP" ]; then
  Z=$WORK/dxvk-zip-$SAFE
  rm -rf "$Z"
  mkdir -p "$Z"
  for p in d3d11 dxgi d3d10 d3d9 d3d8; do for f in "$BUILD"/src/$p/libdxvk_*.so; do "$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-strip" -o "$Z/$(basename "$f")" "$f"; done; done
  printf '{"name": "DXVK %s", "dxvk": "%s", "abi": 1}\n' "${DXVK_TAG#v}" "$DXVK_TAG" > "$Z/meta.json"
  (cd "$Z" && zip -q -j "$OUT/dxvk-$SAFE.zip" meta.json libdxvk_*.so)
  echo "$OUT/dxvk-$SAFE.zip"
  exit 0
fi
for p in d3d11 dxgi d3d10 d3d9 d3d8; do cp "$BUILD"/src/$p/libdxvk_*.so "$OUT/"; done
"$CXX" -std=c++17 -O2 -fPIE -pie -Wall -Wno-extern-c-compat \
  -I"$SRC/include/native/windows" -I"$SRC/include/native/directx" -I"$SRC/include/native" \
  -I"$SRC/include/vulkan/include" "$HERE/cube.cpp" -L"$OUT" -ldxvk_d3d11 -ldxvk_dxgi -lmediandk -landroid \
  "$PAGES" -o "$OUT/hwd3d_cube"
ls "$OUT"
