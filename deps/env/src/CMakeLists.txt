# Sets the minimum version of CMake required to build the native library.

cmake_minimum_required(VERSION 3.5...3.13)

project(env)

add_definitions( -D__STDC_LIMIT_MACROS)
set(CMAKE_CXX_STANDARD 11)

# 兜底：避免 __FILE__ / DWARF 把构建机绝对路径（含用户名）编进 .a / .so。
# 当本项目被 add_subdirectory 时上层已经调用过 add_compile_options，重复添加
# 同样的 prefix-map 是无害的（编译器按顺序应用，无副作用）。当 iOS 通过
# build-deps.sh 直接 cmake 这个 CMakeLists 时，这里的注入是唯一的入口。
include(CheckCCompilerFlag)
get_filename_component(_TT_REPO_ROOT_ENV "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
if (NOT MSVC)
    check_c_compiler_flag("-ffile-prefix-map=/x=." TT_ENV_HAS_FFILE_PREFIX_MAP)
    if (TT_ENV_HAS_FFILE_PREFIX_MAP)
        add_compile_options("-ffile-prefix-map=${_TT_REPO_ROOT_ENV}=.")
    else()
        check_c_compiler_flag("-fdebug-prefix-map=/x=." TT_ENV_HAS_FDEBUG_PREFIX_MAP)
        if (TT_ENV_HAS_FDEBUG_PREFIX_MAP)
            add_compile_options("-fdebug-prefix-map=${_TT_REPO_ROOT_ENV}=.")
        endif()
    endif()
endif()
#SET(CMAKE_BUILE_TYPE RELEASE)
if (CMAKE_BUILD_TYPE MATCHES "Debug")
    if(NOT MSVC)
        SET(CMAKE_C_FLAGS_DEBUG "-g -Wall")
        SET(CMAKE_CXX_FLAGS_DEBUG "-g -Wall")
    endif()
    add_definitions(-D_DEBUG -DDEBUG)
endif()
if(MSVC)
    set(MSVC_DISABLED_WARNINGS_LIST
        "C4061" # enumerator 'identifier' in switch of enum 'enumeration' is not
                # explicitly handled by a case label
                # Disable this because it flags even when there is a default.
        "C4100" # 'exarg' : unreferenced formal parameter
        "C4127" # conditional expression is constant
        "C4200" # nonstandard extension used : zero-sized array in
                # struct/union.
        "C4204" # nonstandard extension used: non-constant aggregate initializer
        "C4221" # nonstandard extension used : 'identifier' : cannot be
                # initialized using address of automatic variable
        "C4242" # 'function' : conversion from 'int' to 'uint8_t',
                # possible loss of data
        "C4244" # 'function' : conversion from 'int' to 'uint8_t',
                # possible loss of data
        "C4267" # conversion from 'size_t' to 'int', possible loss of data
        "C4371" # layout of class may have changed from a previous version of the
                # compiler due to better packing of member '...'
        "C4388" # signed/unsigned mismatch
        "C4296" # '>=' : expression is always true
        "C4350" # behavior change: 'std::_Wrap_alloc...'
        "C4365" # '=' : conversion from 'size_t' to 'int',
                # signed/unsigned mismatch
        "C4389" # '!=' : signed/unsigned mismatch
        "C4464" # relative include path contains '..'
        "C4510" # 'argument' : default constructor could not be generated
        "C4512" # 'argument' : assignment operator could not be generated
        "C4514" # 'function': unreferenced inline function has been removed
        "C4548" # expression before comma has no effect; expected expression with
                # side-effect" caused by FD_* macros.
        "C4610" # struct 'argument' can never be instantiated - user defined
                # constructor required.
        "C4623" # default constructor was implicitly defined as deleted
        "C4625" # copy constructor could not be generated because a base class
                # copy constructor is inaccessible or deleted
        "C4626" # assignment operator could not be generated because a base class
                # assignment operator is inaccessible or deleted
        "C4628" # digraphs not supported with -Ze
        "C4668" # 'symbol' is not defined as a preprocessor macro, replacing with
                # '0' for 'directives'
                # Disable this because GTest uses it everywhere.
        "C4706" # assignment within conditional expression
        "C4710" # 'function': function not inlined
        "C4711" # function 'function' selected for inline expansion
        "C4800" # 'int' : forcing value to bool 'true' or 'false'
                # (performance warning)
        "C4820" # 'bytes' bytes padding added after construct 'member_name'
        "C5026" # move constructor was implicitly defined as deleted
        "C5027" # move assignment operator was implicitly defined as deleted
        "C5045" # Compiler will insert Spectre mitigation for memory load if
                # /Qspectre switch specified
    )
    set(MSVC_LEVEL4_WARNINGS_LIST
        # See https://connect.microsoft.com/VisualStudio/feedback/details/1217660/warning-c4265-when-using-functional-header
        "C4265" # class has virtual functions, but destructor is not virtual
    )
    string(REPLACE "C" " -wd" MSVC_DISABLED_WARNINGS_STR
                            ${MSVC_DISABLED_WARNINGS_LIST})
    string(REPLACE "C" " -w4" MSVC_LEVEL4_WARNINGS_STR
                            ${MSVC_LEVEL4_WARNINGS_LIST})
    set(CMAKE_C_FLAGS   "-utf-8 -EHsc ${MSVC_DISABLED_WARNINGS_STR} ${MSVC_LEVEL4_WARNINGS_STR}")
    set(CMAKE_CXX_FLAGS "-utf-8 -EHsc ${MSVC_DISABLED_WARNINGS_STR} ${MSVC_LEVEL4_WARNINGS_STR}")
    add_compile_options(/wd5039 /wd5204)
else()
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
