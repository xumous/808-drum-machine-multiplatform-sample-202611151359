# Android 808 原生核心（与 core808 共享 DSP / 音序器实现）
cmake_minimum_required(VERSION 3.22.1)
project(android808core C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_library(808core SHARED
        core808.c
        core808_io.c
        jni808_android.c)

target_include_directories(808core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(808core PRIVATE -O2 -fno-fast-math -Wall)
target_link_libraries(808core log)
