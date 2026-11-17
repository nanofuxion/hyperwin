#!/bin/sh
# Builds the libraries behind on-phone machine prep as static archives:
#   wimlib (apply install.wim/esd), ntfs-3g + mkntfs (format and write NTFS), hivex (offline
#   registry), cJSON (drivers.json). Sources are downloaded to build/third_party and patched with
#   tools/third_party/patches so both can address a byte window inside a file ("hwwin:OFF:SIZE:PATH").
# usage: tools/third_party/build.sh android|mac      -> build/third_party/<target>-prep/lib*.a
set -eu
TARGET=${1:-android}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
TP=$ROOT/build/third_party
SRC=$TP/src
OUT=$TP/$TARGET-prep
JOBS=$(sysctl -n hw.ncpu 2>/dev/null || nproc)

WIMLIB=wimlib-1.14.4
NTFS=ntfs-3g_ntfsprogs-2022.10.3
HIVEX=hivex-1.3.24
CJSON=cJSON-1.7.18

fetch() {  # name url
  mkdir -p "$TP/dl" "$SRC"
  [ -f "$TP/dl/$(basename "$2")" ] || curl -fL -o "$TP/dl/$(basename "$2")" "$2"
}
fetch $WIMLIB https://wimlib.net/downloads/$WIMLIB.tar.gz
fetch $NTFS https://tuxera.com/opensource/$NTFS.tgz
fetch $HIVEX https://download.libguestfs.org/hivex/$HIVEX.tar.gz
fetch $CJSON https://github.com/DaveGamble/cJSON/archive/refs/tags/v1.7.18.tar.gz
unpack() {  # dir archive [patch]
  [ -d "$SRC/$1" ] && return
  tar -xzf "$TP/dl/$2" -C "$SRC"
  [ -n "${3:-}" ] && (cd "$SRC" && patch -p0 < "$ROOT/tools/third_party/patches/$3")
}
unpack $WIMLIB $WIMLIB.tar.gz wimlib-1.14.4-window.patch
unpack $NTFS $NTFS.tgz ntfs-3g-2022.10.3-window.patch
unpack $HIVEX $HIVEX.tar.gz
unpack $CJSON v1.7.18.tar.gz

if [ "$TARGET" = android ]; then
  SDK=$(sed -n 's/^sdk.dir=//p' "$ROOT/local.properties")
  NDK=$SDK/ndk/$(sed -n 's/.*ndkVersion = "\(.*\)"/\1/p' "$ROOT/app/build.gradle.kts")
  BIN=$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin
  export CC=$BIN/aarch64-linux-android34-clang AR=$BIN/llvm-ar RANLIB=$BIN/llvm-ranlib
  HOST=--host=aarch64-linux-android
  export CFLAGS="-O2 -fPIC"
else
  export CC=cc AR=ar RANLIB=ranlib
  HOST=
  export CFLAGS="-O2"
fi
mkdir -p "$OUT/obj" "$OUT/include"

# ntfs-3g: library and mkntfs (main renamed so the app can call it).
B=$OUT/ntfs
mkdir -p "$B"
(cd "$B" && [ -f Makefile ] || "$SRC/$NTFS/configure" $HOST --disable-ntfs-3g --disable-ntfsprogs --disable-crypto \
  --without-fuse --enable-static --disable-shared --prefix="$OUT" >cfg.log 2>&1)
make -C "$B/libntfs-3g" -j"$JOBS" >"$B/make.log" 2>&1
make -C "$B/libntfs-3g" install >/dev/null 2>&1 || true  # its rootlibdir hook fails without a .so
make -C "$B/include" install >/dev/null 2>&1
for f in mkntfs attrdef boot sd utils; do
  $CC $CFLAGS -w -c -DHAVE_CONFIG_H -Dmain=hw_mkntfs_main -I"$B" -I"$SRC/$NTFS/include/ntfs-3g" \
    -I"$SRC/$NTFS/ntfsprogs" "$SRC/$NTFS/ntfsprogs/$f.c" -o "$OUT/obj/mkntfs_$f.o"
done
rm -f "$OUT/lib/libmkntfs.a"
$AR rcs "$OUT/lib/libmkntfs.a" "$OUT"/obj/mkntfs_*.o

# wimlib with NTFS-3G apply support.
B=$OUT/wimlib
mkdir -p "$B"
NTFS_LIBS="$OUT/lib/libntfs-3g.a"
[ "$TARGET" = mac ] && NTFS_LIBS="$NTFS_LIBS -framework CoreFoundation"
(cd "$B" && [ -f Makefile ] || "$SRC/$WIMLIB/configure" $HOST --with-ntfs-3g --without-fuse --disable-shared \
  --enable-static --prefix="$OUT" LIBNTFS_3G_CFLAGS="-I$OUT/include -DHAVE_FFS=1 -DHAVE_DAEMON=1 -DHAVE_STRSEP=1" LIBNTFS_3G_LIBS="$NTFS_LIBS" >cfg.log 2>&1)
make -C "$B" -j"$JOBS" libwim.la >"$B/make.log" 2>&1
rm -f "$OUT/lib/libwim.a"   # libtool nests libntfs-3g.a inside its archive; take the objects only
$AR rcs "$OUT/lib/libwim.a" "$B"/src/*.o
cp "$SRC/$WIMLIB/include/wimlib.h" "$OUT/include/"

# hivex lib/ and cJSON, compiled directly (hivex-shim replaces autotools and gnulib).
rm -f "$OUT"/obj/hivex_*.o
for f in handle node offset-list utf16 util value visit write; do
  $CC $CFLAGS -w -c -DHAVE_CONFIG_H -I"$ROOT/tools/third_party/hivex-shim" -I"$SRC/$HIVEX/include" \
    -I"$SRC/$HIVEX/lib" "$SRC/$HIVEX/lib/$f.c" -o "$OUT/obj/hivex_$f.o"
done
$CC $CFLAGS -c "$SRC/$CJSON/cJSON.c" -o "$OUT/obj/cjson.o"
rm -f "$OUT/lib/libhivex.a" "$OUT/lib/libcjson.a"
$AR rcs "$OUT/lib/libhivex.a" "$OUT"/obj/hivex_*.o
$AR rcs "$OUT/lib/libcjson.a" "$OUT/obj/cjson.o"
cp -f "$SRC/$HIVEX/include/hivex.h" "$SRC/$CJSON/cJSON.h" "$OUT/include/"
ls -la "$OUT/lib"/*.a
