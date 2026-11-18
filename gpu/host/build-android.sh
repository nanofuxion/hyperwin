#!/bin/bash
# Builds libhwgpu.so (arm64) for the app: the ring server plus its JNI entry, linked to the
# libgfxstream_backend.so that build-gfxstream.sh builds (run first when missing).
#   gpu/host/build-android.sh [gfxstream output dir]
# Output: gpu/work/android/arm64-v8a/{libhwgpu.so,libgfxstream_backend.so,libc++_shared.so},
# ready to drop into the app's jniLibs.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
GFX=${1:-$HERE/../work/gfx-out}
[ -f "$GFX/lib/arm64-v8a/libgfxstream_backend.so" ] || "$HERE/build-gfxstream.sh"
NDK=${ANDROID_HOME:-$HOME/Library/Android/sdk}/ndk/27.2.12479018
CXX=$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android34-clang++
OUT=$HERE/../work/android/arm64-v8a
mkdir -p "$OUT"
"$CXX" -std=c++17 -O2 -fPIC -shared -Wall -Wextra -Wno-missing-field-initializers \
  -I"$HERE/../include" -I"$HERE/../../include" -I"$GFX/include" "$HERE/hwgpu_host.cpp" "$HERE/hwgpu_jni.cpp" \
  -L"$GFX/lib/arm64-v8a" -lgfxstream_backend -llog -landroid -nostdlib++ -lc++_shared -Wl,-z,max-page-size=16384 \
  -o "$OUT/libhwgpu.so"
cp "$GFX/lib/arm64-v8a/libgfxstream_backend.so" "$GFX/lib/arm64-v8a/libc++_shared.so" "$OUT/"
ls -la "$OUT"
# ring_test for the phone's shell: adb push gpu/work/android/arm64-v8a /data/local/tmp/hwgpu, then
# LD_LIBRARY_PATH=/data/local/tmp/hwgpu /data/local/tmp/hwgpu/ring_test
"$CXX" -std=c++17 -O1 -g -fPIE -pie -Wall -Wextra -Wno-missing-field-initializers \
  -I"$HERE/../include" -I"$HERE/../../include" -I"$GFX/include" "$HERE/ring_test.cpp" \
  -L"$OUT" -lhwgpu -lgfxstream_backend -llog -landroid -nostdlib++ -lc++_shared -o "$OUT/ring_test"
# The wrapper driver core with no fixes (icd/passthrough.cpp), for checking the core on the phone:
#   ANDROID_EMU_VK_LOADER_PATH=/data/local/tmp/hwgpu/libvulkan_passthrough.so ./ring_test
# Real wrappers add a fix module in place of passthrough.cpp. HWGPU_VK_DRIVER picks the driver underneath.
MESA_INC=$HERE/../work/mesa/include   # Vulkan headers; gpu/guest/mesa/build.sh fetches them
"$CXX" -std=c++17 -O2 -fPIC -shared -fvisibility=hidden -Wall -Wextra -DVK_NO_PROTOTYPES -I"$MESA_INC" \
  "$HERE/icd/wrap.cpp" "$HERE/icd/passthrough.cpp" -llog -ldl -nostdlib++ -lc++_shared \
  -Wl,-z,max-page-size=16384 -o "$OUT/libvulkan_passthrough.so"

# libvulkan_bcn.so (icd/bcn.cpp) and the BC compute layer it loads, bcn_layer
# (github.com/Pipetto-crypto/bcn_layer, MIT), built unchanged. Needs glslc and xxd.
BCN=$HERE/../work/bcn_layer
BCN_REV=923623132fbc83c072ef15747ab6e0a8de96cf8c
if [ ! -d "$BCN/.git" ]; then
  git init -q "$BCN"
  git -C "$BCN" fetch -q --depth 1 https://github.com/Pipetto-crypto/bcn_layer.git "$BCN_REV"
  git -C "$BCN" checkout -q FETCH_HEAD
  # The features it strips at device creation lived in a block that had ended, and only requests
  # through pEnabledFeatures were stripped, not through VkPhysicalDeviceFeatures2 (Zink, DXVK).
  git -C "$BCN" apply "$HERE/icd/bcn_layer.patch"
fi
BCN_GEN=$HERE/../work/bcn-build
mkdir -p "$BCN_GEN"
for s in s3tc s3tc_iv rgtc rgtc_iv bc6 bc6_iv bc7 bc7_iv; do
  glslc "$BCN/src/$s.comp" -o "$BCN_GEN/$s.spv"
  (cd "$BCN_GEN" && xxd -i "$s.spv" > "${s}_spv.h")
done
"$CXX" -std=c++17 -O2 -fPIC -shared -I"$BCN_GEN" -I"$BCN/src" -I"$MESA_INC" \
  "$BCN"/src/{bcn_layer,image,buffer,bcn,command_buffer,queue,fence,logger}.cpp \
  -llog -nostdlib++ -lc++_shared -Wl,-z,max-page-size=16384 -o "$OUT/libbcn_layer.so"
"$CXX" -std=c++17 -O2 -fPIC -shared -fvisibility=hidden -Wall -Wextra -DVK_NO_PROTOTYPES -I"$MESA_INC" \
  "$HERE/icd/wrap.cpp" "$HERE/icd/bcn.cpp" -llog -ldl -nostdlib++ -lc++_shared \
  -Wl,-z,max-page-size=16384 -o "$OUT/libvulkan_bcn.so"
# bctest: one BC1 block through a wrapper, read back. ./bctest ./libvulkan_bcn.so
"$CXX" -std=c++17 -O1 -fPIE -pie -Wall -DVK_NO_PROTOTYPES -I"$MESA_INC" "$HERE/icd/bctest.cpp" -ldl \
  -nostdlib++ -lc++_shared -o "$OUT/bctest"

# libvulkan_vortek.so (icd/vortek.cpp): Vortek's Mali workarounds. Vortek's SPIR-V passes,
# shader_inspector.c, are built unchanged from winlator-app (LGPL-2.1) against icd/vortek_shim.
VSRC=$HERE/../work/vortek-src
VREV=ecd5ab2980de79eb955621cef26a3b4672013875
VURL=https://raw.githubusercontent.com/brunodev85/winlator-app/$VREV/app/src/main/cpp
mkdir -p "$VSRC"
for f in vortekrenderer/src/shader_inspector.c vortekrenderer/include/shader_inspector.h \
         vortekrenderer/include/spirv.h winlator/include/arrays.h winlator/src/arrays.c \
         winlator/include/string_utils.h winlator/include/winlator.h; do
  [ -f "$VSRC/$(basename $f)" ] || curl -sfL -o "$VSRC/$(basename $f)" "$VURL/$f"
done
CC=$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android31-clang
for f in shader_inspector arrays; do
  "$CC" -O2 -fPIC -fvisibility=hidden -w -DVK_NO_PROTOTYPES -I"$HERE/icd/vortek_shim" -I"$VSRC" -I"$MESA_INC" \
    -c "$VSRC/$f.c" -o "$VSRC/$f.o"
done
"$CXX" -std=c++17 -O2 -fPIC -shared -fvisibility=hidden -Wall -Wextra -DVK_NO_PROTOTYPES -I"$MESA_INC" \
  "$HERE/icd/wrap.cpp" "$HERE/icd/vortek.cpp" "$VSRC/shader_inspector.o" "$VSRC/arrays.o" \
  -llog -ldl -nostdlib++ -lc++_shared -Wl,-z,max-page-size=16384 -o "$OUT/libvulkan_vortek.so"
# scaledtest: a triangle from SSCALED vertices through a wrapper. ./scaledtest ./libvulkan_vortek.so
SHD=$HERE/../work/shaders
mkdir -p "$SHD"
glslc -mfmt=num "$HERE/icd/scaled.vert" -o "$SHD/scaled.vert.inc"
glslc -mfmt=num "$HERE/icd/scaled.frag" -o "$SHD/scaled.frag.inc"
"$CXX" -std=c++17 -O1 -fPIE -pie -Wall -Wno-missing-field-initializers -DVK_NO_PROTOTYPES -I"$MESA_INC" -I"$SHD" \
  "$HERE/icd/scaledtest.cpp" -ldl -nostdlib++ -lc++_shared -o "$OUT/scaledtest"
# A stand-in libhardware.so (icd/libhardware_stub.c), so drivers built as HAL modules load from the app.
"$CC" -O2 -fPIC -shared -Wall -Wl,-soname,libhardware.so -Wl,-z,max-page-size=16384 \
  "$HERE/icd/libhardware_stub.c" -o "$OUT/libhardware.so"
