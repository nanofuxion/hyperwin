#!/bin/bash
# Builds the Windows Vulkan driver: Mesa's gfxstream encoder with the HyperWin ring backend
# (windows.patch), plus vktest.exe, SweetCider's triangle check, to run inside Windows. The driver
# is ARM64X (gpu/guest/arm64x): ARM64 apps and x64 apps under emulation both load it.
#   gpu/guest/mesa/build.sh [workdir]
# Output in gpu/work/out/: vulkan_hwgpu.dll, vulkan_hwgpu.json, the same as _x86 for 32-bit programs, vktest.exe.
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
cp "$ROOT/gpu/include/"{hwgpu_ring.h,hwgpu_ioctl.h,sgpu_proto.h} "$ROOT/include/"{hw_wddm.h,hww_user.h} "$SRC/src/gfxstream/guest/platform/windows/hwgpu/"

"$HERE/../arm64x/cross.sh" "$WORK/arm64x.ini"
# --gc-sections and vk_util.c's SPIR-V helpers: see the vulkan/meson.build hunk in windows.patch.
[ -d "$WORK/build-x" ] || meson setup "$WORK/build-x" "$SRC" --cross-file "$WORK/arm64x.ini" \
  -Dvulkan-drivers=gfxstream -Dgallium-drivers= -Dplatforms=windows -Dglx=disabled -Degl=disabled \
  -Dgles1=disabled -Dgles2=disabled -Dopengl=false -Dllvm=disabled -Dvalgrind=disabled \
  -Dlibunwind=disabled -Dzstd=disabled -Dmicrosoft-clc=disabled -Dspirv-to-dxil=false \
  -Dbuildtype=debugoptimized "-Dc_link_args=['-static','-Wl,--gc-sections']" "-Dcpp_link_args=['-static','-Wl,--gc-sections']"
ninja -C "$WORK/build-x" src/gfxstream/guest/vulkan/libvulkan_gfxstream.dll

OUT=$WORK/out
mkdir -p "$OUT" "$WORK/shim"
"$T/llvm-strip" -o "$OUT/vulkan_hwgpu.dll" "$WORK/build-x/src/gfxstream/guest/vulkan/libvulkan_gfxstream.dll"
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
# The 32-bit driver, for x86 programs: a plain i686 build of the same source (gpu/guest/mesa/x86.ini).
cat > "$WORK/x86.ini" <<INI
[binaries]
c = '$T/i686-w64-mingw32-clang'
cpp = '$T/i686-w64-mingw32-clang++'
ar = '$T/llvm-ar'
strip = '$T/llvm-strip'
windres = '$T/i686-w64-mingw32-windres'
pkg-config = 'false'
[host_machine]
system = 'windows'
cpu_family = 'x86'
cpu = 'i686'
endian = 'little'
INI
[ -d "$WORK/build-x86" ] || meson setup "$WORK/build-x86" "$SRC" --cross-file "$WORK/x86.ini" \
  -Dvulkan-drivers=gfxstream -Dgallium-drivers= -Dplatforms=windows -Dglx=disabled -Degl=disabled \
  -Dgles1=disabled -Dgles2=disabled -Dopengl=false -Dllvm=disabled -Dvalgrind=disabled \
  -Dlibunwind=disabled -Dzstd=disabled -Dmicrosoft-clc=disabled -Dspirv-to-dxil=false \
  -Dbuildtype=debugoptimized "-Dc_link_args=['-static','-Wl,--gc-sections','-Wl,--kill-at']" "-Dcpp_link_args=['-static','-Wl,--gc-sections','-Wl,--kill-at']"
ninja -C "$WORK/build-x86" src/gfxstream/guest/vulkan/libvulkan_gfxstream.dll
"$T/llvm-strip" -o "$OUT/vulkan_hwgpu_x86.dll" "$WORK/build-x86/src/gfxstream/guest/vulkan/libvulkan_gfxstream.dll"
sed -e 's/vulkan_hwgpu.dll/vulkan_hwgpu_x86.dll/' -e 's/"64"/"32"/' "$OUT/vulkan_hwgpu.json" > "$OUT/vulkan_hwgpu_x86.json"
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
# The same check as an x64 program, which Windows runs under emulation: it loads the ARM64EC half
# of the driver.
"$T/x86_64-w64-mingw32-clang" -std=c11 -O1 -I"$WORK/shim" -I"$SRC/include" -I"$VKTEST" \
  -o "$OUT/vktest-x64.exe" "$VKTEST/vktest.c"
ls -la "$OUT"
