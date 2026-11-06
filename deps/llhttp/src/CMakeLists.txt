# Sets the minimum version of CMake required to build the native library.

cmake_minimum_required(VERSION 2.8...3.13)

project(llhttp)

add_definitions(-D__STDC_LIMIT_MACROS -DHAVE_CONFIG_H)

set(CMAKE_CXX_STANDARD 11)
#SET(CMAKE_BUILE_TYPE RELEASE)
if (CMAKE_BUILD_TYPE MATCHES "Debug")
	SET(CMAKE_C_FLAGS_DEBUG "-g -Wall -Wno-unused-const-variable")
	SET(CMAKE_CXX_FLAGS_DEBUG "-g -Wall -Wno-unused-const-variable")
        add_definitions(-D_DEBUG -DDEBUG)
endif()

add_compile_options(-fPIC)

set(CMAKE_CXX_FLAGS "-fexceptions")

set(BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")


# 输出路径必须带平台 + 架构，与 deps/env、deps/jquic 的约定一致
# （lib/<System>/<Processor>/<Config>）。
#
# 早先这里只有 lib/${CMAKE_BUILD_TYPE}，四个平台共用同一个 libllhttp.a：
# scripts/build-for-linux-and-macos.sh 依次编 linux-x64 → linux-arm64 →
# macos-arm64 → macos-x64，后一个 target 看到 .a 比三个 .c 新就不重编，直接
# 链上一个架构的库，于是报
#   Relocations in generic ELF (EM: 183) / file in wrong format
# EM 183 = aarch64。谁先谁后决定了哪个平台挂，清掉 build/ 也不管用 —— 产物在
# deps/ 里，不在 build/ 里。
set(LIBRARY_OUTPUT_PATH ${CMAKE_CURRENT_SOURCE_DIR}/../lib/${CMAKE_SYSTEM_NAME}/${CMAKE_SYSTEM_PROCESSOR}/${CMAKE_BUILD_TYPE})

INCLUDE_DIRECTORIES(
        ${BASE_DIR}/../include
)

#set(SRC_FILES
#      ${BASE_DIR}/xxxx.cpp
#      ${BASE_DIR}/yyy.cpp
#)
file(GLOB SRC_FILES
      ${BASE_DIR}/*.c
      ${BASE_DIR}/*.cc
)

# message(${SRC_FILES})

# Creates and names a library, sets it as either STATIC
# or SHARED, and provides the relative paths to its source code.
# You can define multiple libraries, and CMake builds them for you.
# Gradle automatically packages shared libraries with your APK.

add_library( # Sets the name of the library.
        llhttp

        # Sets the library as a shared library.
        STATIC

        # Provides a relative path to your source file(s).
        ${SRC_FILES})


# target_link_libraries( llhttp )
