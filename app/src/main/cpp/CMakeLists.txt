cmake_minimum_required(VERSION 3.22)
project(hyperwin C)
set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../..)
# Machine prep libraries come from tools/third_party/build.sh android.
set(PREP ${ROOT}/build/third_party/android-prep)
add_library(hyperwin SHARED jni.c files.c
    ${ROOT}/core/ring.c
    ${ROOT}/core/tpm2.c
    ${ROOT}/core/sha256.c
    ${ROOT}/core/packfat.c
    ${ROOT}/core/bcd_testsign.c
    ${ROOT}/core/machine_prep.c
    ${ROOT}/core/fat32.c
    ${ROOT}/core/udf.c)
target_include_directories(hyperwin PRIVATE ${ROOT}/include ${ROOT}/core ${PREP}/include)
target_compile_options(hyperwin PRIVATE -Wall -Wextra -O2)
target_link_libraries(hyperwin
    ${PREP}/lib/libwim.a ${PREP}/lib/libmkntfs.a ${PREP}/lib/libntfs-3g.a
    ${PREP}/lib/libhivex.a ${PREP}/lib/libcjson.a z android)
target_link_options(hyperwin PRIVATE "-Wl,-z,max-page-size=16384" "-Wl,-z,common-page-size=16384")
