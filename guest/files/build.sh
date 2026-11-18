#!/bin/sh
# Builds build/files/hwfiles.exe (Windows ARM64): phone storage as drives, on WinFsp.
# WinFsp itself (headers and the installer the guest gets) comes from build/third_party/winfsp.
set -eu
cd "$(dirname "$0")/../.."
WINFSP=build/third_party/winfsp
WINFSP_VERSION=2.1.25156
if [ ! -f "$WINFSP/winfsp-$WINFSP_VERSION.msi" ] || [ ! -d "$WINFSP/src/inc" ]; then
  mkdir -p "$WINFSP/src"
  curl -fsSL -o "$WINFSP/winfsp-$WINFSP_VERSION.msi" \
    "https://github.com/winfsp/winfsp/releases/download/v2.1/winfsp-$WINFSP_VERSION.msi"
  curl -fsSL https://github.com/winfsp/winfsp/archive/refs/tags/v2.1.tar.gz | tar xz -C "$WINFSP/src" --strip-components=1
fi
CC=build/third_party/win/llvm-mingw/bin/aarch64-w64-mingw32-clang
mkdir -p build/files
"$CC" -O2 -Wall -Wextra -Wno-missing-field-initializers -Wno-unused-parameter -Wno-unknown-pragmas -Iinclude -Igpu/include \
  -I"$WINFSP/src/inc" -o build/files/hwfiles.exe guest/files/hwfiles.c -ladvapi32
osslsigncode sign -certs build/testcert/hyperwin-pkg-chain.crt -key build/testcert/hyperwin-pkg.key -h sha256 \
  -n "HyperWin files" -in build/files/hwfiles.exe -out build/files/hwfiles.signed.exe >/dev/null
mv build/files/hwfiles.signed.exe build/files/hwfiles.exe
cp "$WINFSP/winfsp-$WINFSP_VERSION.msi" build/files/winfsp.msi
echo build/files/hwfiles.exe
