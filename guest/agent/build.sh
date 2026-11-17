#!/bin/sh
# Builds build/agent/hwagent.exe (Windows ARM64, user mode) with llvm-mingw from tools/third_party.
set -eu
cd "$(dirname "$0")/../.."
CC=build/third_party/win/llvm-mingw/bin/aarch64-w64-mingw32-clang
mkdir -p build/agent
"$CC" -O2 -Wall -Wextra -Wno-missing-field-initializers -Iinclude -o build/agent/hwagent.exe \
  guest/agent/hwagent.c -lntdll -lwtsapi32 -luserenv -liphlpapi -ladvapi32 -lole32 -lshell32
osslsigncode sign -certs build/testcert/hyperwin-pkg-chain.crt -key build/testcert/hyperwin-pkg.key -h sha256 \
  -n "HyperWin agent" -in build/agent/hwagent.exe -out build/agent/hwagent.signed.exe >/dev/null
mv build/agent/hwagent.signed.exe build/agent/hwagent.exe
echo build/agent/hwagent.exe
