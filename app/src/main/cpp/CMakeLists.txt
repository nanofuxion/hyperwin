cmake_minimum_required(VERSION 3.22)
project(hyperwin C)
add_library(hyperwin SHARED jni.c
    ../../../../core/ring.c
    ../../../../core/tpm2.c
    ../../../../core/sha256.c
    ../../../../core/packfat.c
    ../../../../core/bcd_testsign.c)
target_include_directories(hyperwin PRIVATE ../../../../include ../../../../core)
target_compile_options(hyperwin PRIVATE -Wall -Wextra -O2)
target_link_options(hyperwin PRIVATE "-Wl,-z,max-page-size=16384" "-Wl,-z,common-page-size=16384")
