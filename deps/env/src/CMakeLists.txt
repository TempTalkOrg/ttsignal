# Sets the minimum version of CMake required to build the native library.

cmake_minimum_required(VERSION 3.5...3.13)

project(env)

add_definitions( -D__STDC_LIMIT_MACROS)
set(CMAKE_CXX_STANDARD 11)
#SET(CMAKE_BUILE_TYPE RELEASE)
if (CMAKE_BUILD_TYPE MATCHES "Debug")
	SET(CMAKE_C_FLAGS_DEBUG "-g -Wall")
	SET(CMAKE_CXX_FLAGS_DEBUG "-g -Wall")
        add_definitions(-D_DEBUG -DDEBUG)
endif()

add_compile_options(-fPIC)

set(CMAKE_CXX_FLAGS "-fexceptions")
add_compile_options(-Wno-deprecated-declarations)
add_compile_options(-Wno-unused-label)
add_compile_options(-Wno-unused-parameter)
add_compile_options(-Wno-unused-result)
add_compile_options(-Wno-unused-variable)
add_compile_options(-Wno-unused-but-set-variable)
add_compile_options(-Wno-unused-value)
add_compile_options(-Wno-overflow)
add_compile_options(-Wno-unused-function)
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_compile_options(-Wno-nonnull-compare)
        add_compile_options(-Wno-terminate)
endif()

set(BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

set(BASE_BC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/BC")

set(BASE_HTTP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/HTTP")

set(BASE_RTMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/RTMP")

set(LIBRARY_OUTPUT_PATH ${CMAKE_CURRENT_SOURCE_DIR}/../lib/${CMAKE_SYSTEM_NAME}/${CMAKE_SYSTEM_PROCESSOR}/${CMAKE_BUILD_TYPE})

INCLUDE_DIRECTORIES(
        ${BASE_DIR}
        ${BASE_BC_DIR}
        ${BASE_HTTP_DIR}
        ${BASE_RTMP_DIR}
        ${BASE_BC_DIR}/base
        ${BASE_BC_DIR}/build
)

#set(SRC_FILES
#      ${BASE_DIR}/xxxx.cpp
#      ${BASE_DIR}/yyy.cpp
#)
file(GLOB SRC_FILES
      ${BASE_BC_DIR}/*.cpp
      ${BASE_BC_DIR}/base/*.cc
      ${BASE_HTTP_DIR}/*.cpp
      ${BASE_RTMP_DIR}/*.cpp
)

if (WIN32)
    list(APPEND SRC_FILES
        ${BASE_BC_DIR}/win32/os.cpp
    )
endif()

# message(${SRC_FILES})

# Creates and names a library, sets it as either STATIC
# or SHARED, and provides the relative paths to its source code.
# You can define multiple libraries, and CMake builds them for you.
# Gradle automatically packages shared libraries with your APK.

add_library( # Sets the name of the library.
        env

        # Sets the library as a shared library.
        STATIC

        # Provides a relative path to your source file(s).
        ${SRC_FILES})


target_link_libraries( env )
