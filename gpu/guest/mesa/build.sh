#!/bin/bash
# Builds the Windows ARM64 Vulkan driver: Mesa's gfxstream encoder with the HyperWin ring backend
# (windows.patch), plus vktest.exe, SweetCider's triangle check, to run inside Windows.
#   gpu/guest/mesa/build.sh [workdir]
# Output in gpu/work/out/: vulkan_hwgpu.dll, vulkan_hwgpu.json, vktest.exe.
# Needs meson, ninja, mako, pyyaml, and llvm-mingw in build/third_party/win (tools/third_party).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
WORK=${1:-$ROOT/gpu/work}
REV=e0856f57b87dd9731a4b738ea739d0b163a31510   # the Mesa SweetCider's gfxstream-guest builds
SRC=$WORK/mesa
T=$ROOT/build/third_party/win/llvm-mingw/bin
mkdir -p "$WORK"
if [ ! -d "$SRC/.git" ]; then
  git init -q "$SRC"
  git -C "$SRC" fetch -q --depth 1 https://gitlab.freedesktop.org/mesa/mesa.git $REV
  git -C "$SRC" checkout -q FETCH_HEAD
  git -C "$SRC" apply "$HERE/windows.patch"
fi
mkdir -p "$SRC/src/gfxstream/guest/platform/windows/hwgpu"
cp "$ROOT/gpu/include/"{hwgpu_ring.h,hwgpu_ioctl.h,sgpu_proto.h} "$SRC/src/gfxstream/guest/platform/windows/hwgpu/"

cat > "$WORK/win-cross.ini" <<INI
[binaries]
c = '$T/aarch64-w64-mingw32-clang'
cpp = '$T/aarch64-w64-mingw32-clang++'
ar = '$T/llvm-ar'
strip = '$T/llvm-strip'
windres = '$T/aarch64-w64-mingw32-windres'
[host_machine]
system = 'windows'
cpu_family = 'aarch64'
cpu = 'armv8'
endian = 'little'
INI
# --gc-sections and vk_util.c's SPIR-V helpers: see the vulkan/meson.build hunk in windows.patch.
[ -d "$WORK/build" ] || meson setup "$WORK/build" "$SRC" --cross-file "$WORK/win-cross.ini" \
  -Dvulkan-drivers=gfxstream -Dgallium-drivers= -Dplatforms=windows -Dglx=disabled -Degl=disabled \
  -Dgles1=disabled -Dgles2=disabled -Dopengl=false -Dllvm=disabled -Dvalgrind=disabled \
  -Dlibunwind=disabled -Dzstd=disabled -Dmicrosoft-clc=disabled -Dspirv-to-dxil=false \
  -Dbuildtype=debugoptimized -Dc_link_args=-Wl,--gc-sections -Dcpp_link_args=-Wl,--gc-sections
ninja -C "$WORK/build" src/gfxstream/guest/vulkan/libvulkan_gfxstream.dll

OUT=$WORK/out
mkdir -p "$OUT" "$WORK/shim"
"$T/llvm-strip" -o "$OUT/vulkan_hwgpu.dll" "$WORK/build/src/gfxstream/guest/vulkan/libvulkan_gfxstream.dll"
# The loader finds it through HKLM\SOFTWARE\Khronos\Vulkan\Drivers, value = this file's path.
cat > "$OUT/vulkan_hwgpu.json" <<'JSON'
{
    "file_format_version": "1.0.1",
    "ICD": {
        "library_path": ".\\vulkan_hwgpu.dll",
        "library_arch": "64",
        "api_version": "1.1.0"
    }
}
JSON
# vktest loads the driver directly; dlopen and friends map onto LoadLibrary.
cat > "$WORK/shim/dlfcn.h" <<'H'
#include <windows.h>
#define RTLD_LAZY 0
#define RTLD_LOCAL 0
#define dlopen(p, f) ((void *)LoadLibraryA(p))
#define dlsym(h, s) ((void *)GetProcAddress((HMODULE)(h), (s)))
#define dlerror() "LoadLibrary failed"
H
VKTEST=$ROOT/../sweetcider-gpu/tools/gfxstream-guest/vktest
"$T/aarch64-w64-mingw32-clang" -std=c11 -O1 -I"$WORK/shim" -I"$SRC/include" -I"$VKTEST" \
  -o "$OUT/vktest.exe" "$VKTEST/vktest.c"
ls -la "$OUT"
