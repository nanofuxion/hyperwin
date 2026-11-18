#!/bin/bash
# Builds libgfxstream_backend.so for the phone from gfxstream with host/gfxstream.patch: SweetCider's
# Android changes (non-blocking pipe reads, a pipe per guest thread) and HyperWin's allocator hook,
# which puts host-visible Vulkan memory in the heap shared with Windows, and the export of a
# ColorBuffer's AHardwareBuffer (stream_renderer_resource_get_ahb) for scanout.
#   gpu/host/build-gfxstream.sh     -> gpu/work/gfx-out/{include,lib/arm64-v8a}
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=$HERE/../work
SRC=$WORK/gfxstream
REV=e4fae080c3d0effa313e3e036041426d178a90f3
NDK=${ANDROID_HOME:-$HOME/Library/Android/sdk}/ndk/27.2.12479018
TC=$NDK/toolchains/llvm/prebuilt/darwin-x86_64
if [ ! -d "$SRC/.git" ]; then
  git init -q "$SRC"
  git -C "$SRC" fetch -q --depth 1 https://github.com/google/gfxstream.git $REV
  git -C "$SRC" checkout -q FETCH_HEAD
  git -C "$SRC" apply "$HERE/gfxstream.patch"
fi
cat > "$WORK/gfx-android.ini" <<INI
[binaries]
c = '$TC/bin/aarch64-linux-android31-clang'
cpp = '$TC/bin/aarch64-linux-android31-clang++'
ar = '$TC/bin/llvm-ar'
strip = '$TC/bin/llvm-strip'
[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'armv8'
endian = 'little'
INI
ARGS="['-DANDROID','-DVK_USE_PLATFORM_ANDROID_KHR','-idirafter$SRC/third_party/android/include']"
[ -d "$WORK/gfx-build" ] || meson setup "$WORK/gfx-build" "$SRC" --cross-file "$WORK/gfx-android.ini" \
  -Dgfxstream-build=host -Dbuildtype=release -Ddecoders=gles,vulkan,composer -Dlog-level=error \
  "-Dc_args=$ARGS" "-Dcpp_args=$ARGS" "-Dcpp_link_args=['-Wl,-z,max-page-size=16384']"
ninja -C "$WORK/gfx-build" host/libgfxstream_backend.so
OUT=$WORK/gfx-out
mkdir -p "$OUT/lib/arm64-v8a" "$OUT/include/gfxstream"
"$TC/bin/llvm-strip" -o "$OUT/lib/arm64-v8a/libgfxstream_backend.so" "$WORK/gfx-build/host/libgfxstream_backend.so"
cp "$SRC"/host/include/gfxstream/virtio-gpu-gfxstream-renderer*.h "$OUT/include/gfxstream/"
cp "$TC/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so" "$OUT/lib/arm64-v8a/"
ls -la "$OUT/lib/arm64-v8a"
