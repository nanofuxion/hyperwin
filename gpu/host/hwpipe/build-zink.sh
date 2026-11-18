#!/bin/bash
# Builds Mesa's Zink for Android (opengl/README.md, phase 1) from the same Mesa revision as the Windows side,
# and zink_test, which opens Zink on the phone's Vulkan driver through the Gallium interface and reads a pixel
# back. Gallium zink only, no window system. The static libraries stay in gpu/work/mesa-android-build for the
# pipe host (phase 2) to link.
#   gpu/host/hwpipe/build-zink.sh [run]       with "run", pushes zink_test to the phone and runs it
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$HERE/../../..
WORK=$ROOT/gpu/work
REV=e0856f57b87dd9731a4b738ea739d0b163a31510
NDK=${ANDROID_HOME:-$HOME/Library/Android/sdk}/ndk/27.2.12479018
TC=$NDK/toolchains/llvm/prebuilt/darwin-x86_64
M=$WORK/mesa-android
B=$WORK/mesa-android-build
if [ ! -d "$M" ]; then
  git -C "$WORK/mesa" worktree add --detach "$M" "$REV"
  git -C "$M" apply "$HERE/zink-android.patch"   # ZINK_VULKAN_LIB names the Vulkan library to load
fi
cat > "$WORK/mesa-android.ini" <<INI
[binaries]
c = '$TC/bin/aarch64-linux-android31-clang'
cpp = '$TC/bin/aarch64-linux-android31-clang++'
ar = '$TC/bin/llvm-ar'
strip = '$TC/bin/llvm-strip'
pkg-config = 'false'
[built-in options]
c_link_args = ['-Wl,-z,max-page-size=16384']
cpp_link_args = ['-Wl,-z,max-page-size=16384']
[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'armv8'
endian = 'little'
INI
[ -d "$B" ] || meson setup "$B" "$M" --cross-file "$WORK/mesa-android.ini" -Dgallium-drivers=zink -Dvulkan-drivers= \
  -Dplatforms= -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dllvm=disabled -Dvalgrind=disabled -Dlibunwind=disabled \
  -Dzstd=disabled -Dopengl=true -Dgles1=disabled -Dgles2=enabled -Dshared-glapi=enabled -Dbuild-tests=false -Dtools= \
  -Dbuildtype=release
LIBS=(src/gallium/drivers/zink/libzink.a src/gallium/auxiliary/libgallium.a src/gallium/auxiliary/libgalliumvl_stub.a
  src/compiler/nir/libnir.a src/compiler/spirv/libvtn.a src/compiler/libcompiler.a src/compiler/glsl/libglsl_util.a
  src/vulkan/util/libvulkan_util.a src/util/libmesa_util.a src/util/libmesa_util_simd.a src/util/blake3/libblake3.a
  src/util/libxmlconfig.a src/util/libparson.a src/c11/impl/libmesa_util_c11.a)
ninja -C "$B" "${LIBS[@]}" subprojects/expat-2.5.0/libexpat.so
OUT=$WORK/android/zink
mkdir -p "$OUT"
L=(); for f in "${LIBS[@]}"; do L+=("$B/$f"); done
"$TC/bin/aarch64-linux-android31-clang++" -O2 -g -x c -c "$HERE/zink_test.c" -o "$OUT/zink_test.o" \
  -I"$M/src/gallium/include" -I"$M/include" -I"$M/src" -I"$M/src/gallium/auxiliary" -I"$B/src"
# --build-id: Zink reads its own build id for the pipeline cache key and crashes without one.
"$TC/bin/aarch64-linux-android31-clang++" -o "$OUT/zink_test" "$OUT/zink_test.o" -Wl,--start-group "${L[@]}" -Wl,--end-group \
  "$B/subprojects/expat-2.5.0/libexpat.so" -static-libstdc++ -lm -ldl -llog -lz -landroid -Wl,-z,max-page-size=16384 -Wl,--build-id=sha1
cp "$B/subprojects/expat-2.5.0/libexpat.so" "$OUT/"
if [ "${1:-}" = run ]; then
  adb push "$OUT/zink_test" "$OUT/libexpat.so" /data/local/tmp/ >/dev/null
  adb shell "cd /data/local/tmp && chmod +x zink_test && LD_LIBRARY_PATH=. ZINK_VULKAN_LIB=libvulkan.so ./zink_test"
fi
