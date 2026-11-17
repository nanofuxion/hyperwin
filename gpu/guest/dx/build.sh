#!/bin/bash
# Builds the Direct3D and OpenGL layers that run on top of vulkan_hwgpu.dll, as ARM64X DLLs: one
# file holds native ARM64 code for ARM64 apps and ARM64EC code for x64 apps under emulation, so the
# same DLL works in both (llvm-mingw's -marm64x). 32-bit x86 apps are not covered.
#   gpu/guest/dx/build.sh [workdir] [dxvk|sarek|vkd3d|zink ...]
# Output in gpu/work/out/dx/:
#   dxvk/   d3d8 d3d9 d3d10core d3d11 dxgi  (DXVK: Direct3D 8 to 11)
#   sarek/  the same from DXVK-Sarek: DXVK 1.10 carried on for Vulkan 1.1/1.2 and mobile GPUs
#   vkd3d/  d3d12 d3d12core                 (vkd3d-proton: Direct3D 12, needs DXVK's dxgi)
#   zink/   opengl32 libgallium_wgl         (Mesa Zink: OpenGL)
# Needs meson, ninja, glslangValidator, bison 3, mako, pyyaml, and llvm-mingw in build/third_party/win.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
WORK=${1:-$ROOT/gpu/work}
shift || true
PARTS=${*:-dxvk sarek vkd3d zink}
T=$ROOT/build/third_party/win/llvm-mingw/bin
DXVK_TAG=v3.1.1
VKD3D_TAG=v3.1
SAREK_TAG=v1.14.1
OUT=$WORK/out/dx
mkdir -p "$OUT"

"$ROOT/gpu/guest/arm64x/cross.sh" "$WORK/arm64x.ini"

fetch() {  # url tag dir
  [ -d "$3/.git" ] || git clone -q --depth 1 --branch "$2" --recurse-submodules --shallow-submodules "$1" "$3"
}

for part in $PARTS; do case $part in
dxvk)
  fetch https://github.com/doitsujin/dxvk.git $DXVK_TAG "$WORK/dxvk"
  [ -d "$WORK/dxvk-build" ] || meson setup "$WORK/dxvk-build" "$WORK/dxvk" --cross-file "$WORK/arm64x.ini" \
    --buildtype release -Dbuild_id=false
  ninja -C "$WORK/dxvk-build"
  mkdir -p "$OUT/dxvk"
  for d in d3d8 d3d9 d3d10core d3d11 dxgi; do
    "$T/llvm-strip" -o "$OUT/dxvk/$d.dll" "$(find "$WORK/dxvk-build/src" -name "$d.dll" | head -1)"
  done ;;
sarek)
  fetch https://github.com/pythonlover02/DXVK-Sarek.git $SAREK_TAG "$WORK/dxvk-sarek"
  # libc++ wants <algorithm> spelled out where Sarek uses std::transform.
  git -C "$WORK/dxvk-sarek" apply --check "$HERE/sarek.patch" 2>/dev/null && git -C "$WORK/dxvk-sarek" apply "$HERE/sarek.patch"
  [ -d "$WORK/sarek-build" ] || meson setup "$WORK/sarek-build" "$WORK/dxvk-sarek" --cross-file "$WORK/arm64x.ini" \
    --buildtype release -Dbuild_id=false
  ninja -C "$WORK/sarek-build"
  rm -rf "$OUT/sarek"
  mkdir -p "$OUT/sarek"
  for f in $(find "$WORK/sarek-build/src" -name '*.dll'); do "$T/llvm-strip" -o "$OUT/sarek/$(basename "$f")" "$f"; done ;;
vkd3d)
  fetch https://github.com/HansKristian-Work/vkd3d-proton.git $VKD3D_TAG "$WORK/vkd3d-proton"
  [ -d "$WORK/vkd3d-build" ] || meson setup "$WORK/vkd3d-build" "$WORK/vkd3d-proton" --cross-file "$WORK/arm64x.ini" \
    --buildtype release -Denable_tests=false -Denable_extras=false
  ninja -C "$WORK/vkd3d-build"
  mkdir -p "$OUT/vkd3d"
  for d in d3d12 d3d12core; do
    "$T/llvm-strip" -o "$OUT/vkd3d/$d.dll" "$(find "$WORK/vkd3d-build" -name "$d.dll" | head -1)"
  done ;;
zink)
  # Same Mesa tree as the Vulkan driver (gpu/guest/mesa/build.sh fetches it). Zink is OpenGL on
  # Vulkan; the WGL front end gives opengl32.dll, which loads libgallium_wgl.dll beside it.
  [ -d "$WORK/mesa/.git" ] || "$ROOT/gpu/guest/mesa/build.sh" "$WORK"
  # The GLSL compiler's grammar needs bison 3; macOS ships 2.3.
  [ -d /opt/homebrew/opt/bison/bin ] && export PATH=/opt/homebrew/opt/bison/bin:$PATH
  [ -d "$WORK/zink-build" ] || meson setup "$WORK/zink-build" "$WORK/mesa" --cross-file "$WORK/arm64x.ini" \
    -Dvulkan-drivers= -Dgallium-drivers=zink -Dplatforms=windows -Dglx=disabled -Degl=disabled \
    -Dgles1=disabled -Dgles2=disabled -Dopengl=true -Dllvm=disabled -Dvalgrind=disabled \
    -Dlibunwind=disabled -Dzstd=disabled -Dmicrosoft-clc=disabled -Dspirv-to-dxil=false \
    -Dbuildtype=release -Db_ndebug=true -Dzlib:default_library=static
  ninja -C "$WORK/zink-build"
  mkdir -p "$OUT/zink"
  for d in opengl32 libgallium_wgl; do
    "$T/llvm-strip" -o "$OUT/zink/$d.dll" "$(find "$WORK/zink-build/src" -name "$d.dll" | head -1)"
  done ;;
esac; done
ls -laR "$OUT"
