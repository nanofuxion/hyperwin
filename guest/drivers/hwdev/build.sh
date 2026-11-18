#!/bin/sh
# Builds build/hwdev/hwdev.exe (Windows ARM64), which install-driver.ps1 uses to add root devices.
set -eu
cd "$(dirname "$0")/../../.."
CC=build/third_party/win/llvm-mingw/bin/aarch64-w64-mingw32-clang
mkdir -p build/hwdev
"$CC" -O2 -Wall -Wextra -municode -o build/hwdev/hwdev.exe guest/drivers/hwdev/hwdev.c -lsetupapi
osslsigncode sign -certs build/testcert/hyperwin-pkg-chain.crt -key build/testcert/hyperwin-pkg.key -h sha256 \
  -n "HyperWin devices" -in build/hwdev/hwdev.exe -out build/hwdev/hwdev.signed.exe >/dev/null
mv build/hwdev/hwdev.signed.exe build/hwdev/hwdev.exe
echo build/hwdev/hwdev.exe
