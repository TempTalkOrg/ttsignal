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


set(LIBRARY_OUTPUT_PATH ${CMAKE_CURRENT_SOURCE_DIR}/../lib/${CMAKE_BUILD_TYPE})

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
