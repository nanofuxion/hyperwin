#!/bin/bash
# Builds the Khronos Vulkan loader (vulkan-1.dll) for Windows ARM64. Windows ships none, and apps
# link to it, not to a driver: it finds vulkan_hwgpu.dll through HKLM\SOFTWARE\Khronos\Vulkan\Drivers.
# Output: gpu/work/out/vulkan-1.dll. Needs cmake, ninja and llvm-mingw (tools/third_party).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
WORK=$ROOT/gpu/work
TAG=v1.4.304
MINGW=$ROOT/build/third_party/win/llvm-mingw
for repo in Vulkan-Headers Vulkan-Loader; do
  [ -d "$WORK/$repo" ] || git clone -q --depth 1 --branch $TAG https://github.com/KhronosGroup/$repo.git "$WORK/$repo"
done
cat > "$WORK/mingw-arm64.cmake" <<CMAKE
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR ARM64)
set(CMAKE_C_COMPILER $MINGW/bin/aarch64-w64-mingw32-clang)
set(CMAKE_CXX_COMPILER $MINGW/bin/aarch64-w64-mingw32-clang++)
set(CMAKE_RC_COMPILER $MINGW/bin/aarch64-w64-mingw32-windres)
set(CMAKE_ASM_COMPILER $MINGW/bin/aarch64-w64-mingw32-clang)
set(CMAKE_FIND_ROOT_PATH $MINGW/aarch64-w64-mingw32)
CMAKE
cmake -S "$WORK/Vulkan-Headers" -B "$WORK/vk-headers-build" -G Ninja -DCMAKE_INSTALL_PREFIX="$WORK/vk-headers" >/dev/null
cmake --install "$WORK/vk-headers-build" >/dev/null
# USE_GAS: the trampolines for unknown device functions, as GNU assembly (no armasm64 here).
cmake -S "$WORK/Vulkan-Loader" -B "$WORK/vk-loader-build" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$WORK/mingw-arm64.cmake" \
  -DCMAKE_BUILD_TYPE=Release -DVulkanHeaders_DIR="$WORK/vk-headers/share/cmake/VulkanHeaders" \
  -DUSE_GAS=ON -DUSE_MASM=OFF -DBUILD_TESTS=OFF >/dev/null
ninja -C "$WORK/vk-loader-build"
mkdir -p "$WORK/out"
"$MINGW/bin/llvm-strip" -o "$WORK/out/vulkan-1.dll" "$WORK/vk-loader-build/loader/vulkan-1.dll"
echo "$WORK/out/vulkan-1.dll"
