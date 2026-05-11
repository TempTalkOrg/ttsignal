# For more information about using CMake with Android Studio, read the
# documentation: https://d.android.com/studio/projects/add-native-code.html.
# For more examples on how to use CMake, see https://github.com/android/ndk-samples.

# Sets the minimum CMake version required for this project.
cmake_minimum_required(VERSION 3.22.1)

# Declares the project name. The project name can be accessed via ${ PROJECT_NAME},
# Since this is the top level CMakeLists.txt, the project name is also accessible
# with ${CMAKE_PROJECT_NAME} (both CMake variables are in-sync within the top level
# build script scope).
project("signal")

set(BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

# 把构建机绝对路径前缀替换成相对路径，避免 __FILE__ / __builtin_FILE() / DWARF
# 把开发者用户名（/Users/<name>/... 或 /home/<name>/...）和仓库位置编进 .so。
# 必须在 add_subdirectory(deps/...) 之前调用以传给 deps。
# Android NDK r21+ 自带 clang 都支持 -ffile-prefix-map；为统一起见仍做检测。
include(CheckCCompilerFlag)
get_filename_component(_TT_REPO_ROOT "${BASE_DIR}/../../../../.." ABSOLUTE)
if (NOT MSVC)
    check_c_compiler_flag("-ffile-prefix-map=/x=." TT_HAS_FFILE_PREFIX_MAP)
    if (TT_HAS_FFILE_PREFIX_MAP)
        add_compile_options("-ffile-prefix-map=${_TT_REPO_ROOT}=.")
    else()
        check_c_compiler_flag("-fdebug-prefix-map=/x=." TT_HAS_FDEBUG_PREFIX_MAP)
        if (TT_HAS_FDEBUG_PREFIX_MAP)
            add_compile_options("-fdebug-prefix-map=${_TT_REPO_ROOT}=.")
        endif()
    endif()
endif()

add_subdirectory(${BASE_DIR}/../../../../../deps/env/src deps/env.dir)
add_subdirectory(${BASE_DIR}/../../../../../deps/boringssl/src deps/boringssl.dir)
add_subdirectory(${BASE_DIR}/../../../../../deps/jquic deps/jquic.dir)

add_definitions(-D__STDC_LIMIT_MACROS -DHAVE_PTHREAD -DCR_USE_FALLBACKS_FOR_GCC_WITH_LIBCXX
        -DNOMINMAX
        -DXQC_ENABLE_BBR2
        -DXQC_ENABLE_RENO
        -DXQC_ENABLE_UNLIMITED
        -DXQC_ENABLE_COPA)

add_compile_options(-fPIC)

set(CMAKE_CXX_STANDARD 14)

if (CMAKE_BUILD_TYPE MATCHES "Debug")
    SET(LIB_NAME ttsignal )
    SET(CMAKE_C_FLAGS_DEBUG "-g -Wall")
    SET(CMAKE_CXX_FLAGS_DEBUG "-g -Wall")
    add_definitions(-D_DEBUG -DDEBUG)
    add_compile_options(-Wno-deprecated-declarations)
    add_compile_options(-Wno-unused-label)
    add_compile_options(-Wno-unused-parameter)
    add_compile_options(-Wno-unused-result)
    add_compile_options(-Wno-unused-variable)
    add_compile_options(-Wno-unused-but-set-variable)
    add_compile_options(-Wno-conversion-null)
else()
    SET(LIB_NAME ttsignal )
    SET(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -O3 -DNDEBUG")
    SET(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -DNDEBUG")
    add_compile_options(-Wno-deprecated-declarations)
    add_compile_options(-Wno-unused-label)
    add_compile_options(-Wno-unused-parameter)
    add_compile_options(-Wno-unused-result)
    add_compile_options(-Wno-unused-variable)
    add_compile_options(-Wno-unused-but-set-variable)
    add_compile_options(-Wno-conversion-null)
endif()

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fexceptions")

# 添加普通 C++ 头文件路径到包含目录
include_directories(
        ${BASE_DIR}/../../../../../deps/env/src
        ${BASE_DIR}/../../../../../deps/boringssl/src/include
        ${BASE_DIR}/../../../../../deps/jquic/include
        ${BASE_DIR}/../../../../../src/cpp
)

file(GLOB JNI_SRC_FILES
        ${BASE_DIR}/../../../../../src/cpp/*.cpp
        ${BASE_DIR}/../../../../../src/cpp/http-parser/http_parser.c
        ${BASE_DIR}/../../../../../src/cpp/jni/*.cpp
)

# Creates and names a library, sets it as either STATIC
# or SHARED, and provides the relative paths to its source code.
# You can define multiple libraries, and CMake builds them for you.
# Gradle automatically packages shared libraries with your APK.
#
# In this top level CMakeLists.txt, ${CMAKE_PROJECT_NAME} is used to define
# the target library name; in the sub-module's CMakeLists.txt, ${PROJECT_NAME}
# is preferred for the same purpose.
#
# In order to load a library into your app from Java/Kotlin, you must call
# System.loadLibrary() and pass the name of the library defined here;
# for GameActivity/NativeActivity derived applications, the same library name must be
# used in the AndroidManifest.xml file.
add_library(${CMAKE_PROJECT_NAME} SHARED
        # List C/C++ source files with relative paths to this CMakeLists.txt.
        ${JNI_SRC_FILES})

# 为 AArch64 架构添加 16KB 页面支持
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64" OR CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a")
    add_definitions(-DANDROID_16KB_PAGESIZE)
    target_link_options(${CMAKE_PROJECT_NAME} PRIVATE "-Wl,-z,max-page-size=16384")
endif()

# Specifies libraries CMake should link to your target library. You
# can link libraries from various origins, such as libraries defined in this
# build script, prebuilt third-party libraries, or Android system libraries.
target_link_libraries(${CMAKE_PROJECT_NAME}
        # List libraries link to the target library
        env
        jquic
        ssl
        crypto
        android
        log)
