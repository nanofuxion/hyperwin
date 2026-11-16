#!/bin/bash
set -euo pipefail

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
FW="$ROOT/firmware/avf-edk2"
EDK2_SOURCE=${EDK2_SOURCE:-/path/to/edk2}
LLVM_BIN=${LLVM_BIN:-/opt/homebrew/opt/llvm/bin}
GCC_PREFIX=${GCC5_AARCH64_PREFIX:-/opt/homebrew/bin/aarch64-elf-}

if [ ! -f "$EDK2_SOURCE/ArmVirtPkg/ArmVirtKvmTool.dsc" ]; then
    echo "Set EDK2_SOURCE to an EDK2 checkout with ArmVirtPkg/ArmVirtKvmTool.dsc" >&2
    exit 2
fi
if [ ! -x "$EDK2_SOURCE/BaseTools/BinWrappers/PosixLike/build" ]; then
    echo "EDK2 BaseTools build wrapper is missing; build BaseTools in EDK2_SOURCE first" >&2
    exit 2
fi

mkdir -p "$FW/out" "$FW/Build" "$FW/conf"
cd "$EDK2_SOURCE"
export PATH="$LLVM_BIN:$EDK2_SOURCE/BaseTools/BinWrappers/PosixLike:$EDK2_SOURCE/BaseTools/Bin/Darwin-arm64:$PATH"
export PYTHON_COMMAND=python3
export WORKSPACE="$EDK2_SOURCE"
export EDK_TOOLS_PATH="$EDK2_SOURCE/BaseTools"
export CONF_PATH="$FW/conf"
export EDK2_OUT_DIR="$FW/Build"
export GCC5_AARCH64_PREFIX="$GCC_PREFIX"

cp "$EDK2_SOURCE/Conf/target.txt" "$CONF_PATH/target.txt"
cp "$EDK2_SOURCE/Conf/tools_def.txt" "$CONF_PATH/tools_def.txt"
cp "$EDK2_SOURCE/Conf/build_rule.txt" "$CONF_PATH/build_rule.txt"
python3 - "$CONF_PATH/tools_def.txt" <<'PY'
import pathlib, sys
p = pathlib.Path(sys.argv[1])
s = p.read_text()
old_cc = "RELEASE_GCC5_AARCH64_CC_FLAGS    = DEF(GCC5_AARCH64_CC_FLAGS) -flto -Wno-unused-but-set-variable -Wno-unused-const-variable"
old_ld = "RELEASE_GCC5_AARCH64_DLINK_FLAGS = DEF(GCC5_AARCH64_DLINK_FLAGS) -flto -Os -L$(WORKSPACE)/ArmPkg/Library/GccLto -llto-aarch64 -Wl,-plugin-opt=-pass-through=-llto-aarch64 -Wno-lto-type-mismatch"
if old_cc not in s or old_ld not in s:
    raise SystemExit("EDK2 tools_def.txt does not match expected GCC5 AARCH64 release definitions")
s = s.replace(old_cc, "RELEASE_GCC5_AARCH64_CC_FLAGS    = DEF(GCC5_AARCH64_CC_FLAGS) -Os -Wno-unused-but-set-variable -Wno-unused-const-variable -Wno-error=maybe-uninitialized")
s = s.replace(old_ld, "RELEASE_GCC5_AARCH64_DLINK_FLAGS = DEF(GCC5_AARCH64_DLINK_FLAGS) -Os")
p.write_text(s)
PY
set +u
. ./edksetup.sh
set -u

if ! build -a AARCH64 -t GCC5 -b RELEASE \
    -p ArmVirtPkg/ArmVirtKvmTool.dsc \
    -D EDK2_OUT_DIR="$EDK2_OUT_DIR" > "$FW/out/build.log" 2>&1; then
    tail -n 120 "$FW/out/build.log" >&2
    exit 1
fi

FD=$(find "$EDK2_OUT_DIR" -type f -name KVMTOOL_EFI.fd -print -quit)
if [ -z "$FD" ]; then
    echo "Build succeeded but KVMTOOL_EFI.fd was not found under $EDK2_OUT_DIR" >&2
    exit 1
fi
cp "$FD" "$FW/out/avf-kvmtool-aarch64.fd"
file "$FW/out/avf-kvmtool-aarch64.fd"
shasum -a 256 "$FW/out/avf-kvmtool-aarch64.fd"
