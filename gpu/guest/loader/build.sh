#!/bin/bash
# Builds the Khronos Vulkan loader (vulkan-1.dll) for Windows as ARM64X, so ARM64 apps and x64 apps
# under emulation load the same file. Windows ships none, and apps link to it, not to a driver: it
# finds vulkan_hwgpu.dll through HKLM\SOFTWARE\Khronos\Vulkan\Drivers.
# Output: gpu/work/out/vulkan-1.dll. Needs cmake, ninja and llvm-mingw (tools/third_party).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
WORK=$ROOT/gpu/work
TAG=v1.4.304
MINGW=$ROOT/build/third_party/win/llvm-mingw
for repo in Vulkan-Headers Vulkan-Loader; do
  [ -d "$WORK/$repo" ] || git clone -q --depth 1 --branch $TAG https://github.com/KhronosGroup/$repo.git "$WORK/$repo"
done
X=$ROOT/gpu/guest/arm64x
cat > "$WORK/mingw-arm64x.cmake" <<CMAKE
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR ARM64)
set(CMAKE_C_COMPILER $X/clang)
set(CMAKE_CXX_COMPILER $X/clang++)
set(CMAKE_RC_COMPILER $MINGW/bin/arm64ec-w64-mingw32-windres)
set(CMAKE_FIND_ROOT_PATH $MINGW/aarch64-w64-mingw32)
set(CMAKE_C_FLAGS_INIT -march=armv8.2-a)
set(CMAKE_SHARED_LINKER_FLAGS_INIT -static)
CMAKE
cmake -S "$WORK/Vulkan-Headers" -B "$WORK/vk-headers-build" -G Ninja -DCMAKE_INSTALL_PREFIX="$WORK/vk-headers" >/dev/null
cmake --install "$WORK/vk-headers-build" >/dev/null
# No assembly: its trampolines for extension functions the loader does not know are single
# architecture. Known functions, which DXVK, vkd3d-proton and Zink use, need none.
cmake -S "$WORK/Vulkan-Loader" -B "$WORK/vk-loader-x" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$WORK/mingw-arm64x.cmake" \
  -DCMAKE_BUILD_TYPE=Release -DVulkanHeaders_DIR="$WORK/vk-headers/share/cmake/VulkanHeaders" \
  -DUSE_GAS=OFF -DUSE_MASM=OFF -DBUILD_TESTS=OFF >/dev/null
ninja -C "$WORK/vk-loader-x"
mkdir -p "$WORK/out"
"$MINGW/bin/llvm-strip" -o "$WORK/out/vulkan-1.dll" "$WORK/vk-loader-x/loader/vulkan-1.dll"
echo "$WORK/out/vulkan-1.dll"
# The 32-bit loader, for x86 programs (SysWOW64\vulkan-1.dll): an ordinary i686 build.
cat > "$WORK/mingw-i686.cmake" <<CMAKE
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR X86)
set(CMAKE_C_COMPILER $MINGW/bin/i686-w64-mingw32-clang)
set(CMAKE_CXX_COMPILER $MINGW/bin/i686-w64-mingw32-clang++)
set(CMAKE_RC_COMPILER $MINGW/bin/i686-w64-mingw32-windres)
set(CMAKE_FIND_ROOT_PATH $MINGW/i686-w64-mingw32)
set(CMAKE_SHARED_LINKER_FLAGS_INIT -static)
CMAKE
cmake -S "$WORK/Vulkan-Loader" -B "$WORK/vk-loader-x86" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$WORK/mingw-i686.cmake" \
  -DCMAKE_BUILD_TYPE=Release -DVulkanHeaders_DIR="$WORK/vk-headers/share/cmake/VulkanHeaders" \
  -DUSE_GAS=OFF -DUSE_MASM=OFF -DBUILD_TESTS=OFF >/dev/null
ninja -C "$WORK/vk-loader-x86"
"$MINGW/bin/llvm-strip" -o "$WORK/out/vulkan-1_x86.dll" "$WORK/vk-loader-x86/loader/vulkan-1.dll"
echo "$WORK/out/vulkan-1_x86.dll"
