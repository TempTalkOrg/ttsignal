# Sets the minimum version of CMake required to build the native library.

cmake_minimum_required(VERSION 3.10)
if(POLICY CMP0048)
  cmake_policy(SET CMP0048 NEW)
endif()

project(ttsignal)

option(TARGET_PLATFORM_LINUX "Target platform is linux" OFF)
option(BUILD_JNI "Build JNI library" OFF)
option(BUILD_NODE_ADDON "Build Node.js addon" ON)

if(TARGET_PLATFORM_LINUX)
    set(TARGET_OS_LINUX TRUE)
elseif(UNIX AND NOT APPLE)
    set(TARGET_OS_LINUX TRUE)
endif()

set(BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

add_custom_target(touch_utils ALL
    COMMAND ${CMAKE_COMMAND} -E touch "${BASE_DIR}/cpp/Utils.cpp"
    COMMENT "Touching Utils.cpp to update build timestamp"
)

add_subdirectory(${BASE_DIR}/../deps/env/src deps/env.dir)
add_subdirectory(${BASE_DIR}/../deps/boringssl/src deps/boringssl.dir)
add_subdirectory(${BASE_DIR}/../deps/jquic deps/jquic.dir)

add_definitions(-D__STDC_LIMIT_MACROS -DHAVE_PTHREAD -DCR_USE_FALLBACKS_FOR_GCC_WITH_LIBCXX 
        -DNOMINMAX
        -DXQC_ENABLE_BBR2
        -DXQC_ENABLE_RENO
        -DXQC_ENABLE_UNLIMITED
        -DXQC_ENABLE_COPA)


set(CMAKE_CXX_STANDARD 20)


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
    add_compile_options(-Wno-deprecated-declarations)
    add_compile_options(-Wno-unused-label)
    add_compile_options(-Wno-unused-parameter)
    add_compile_options(-Wno-unused-result)
    add_compile_options(-Wno-unused-variable)
    add_compile_options(-Wno-unused-but-set-variable)
    add_compile_options(-Wno-conversion-null)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fexceptions")
endif()

SET(LIB_NAME ttsignal )
if(MSVC)
    SET(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} /D_DEBUG /DDEBUG")
    SET(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} /D_DEBUG /DDEBUG")
    SET(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} /O2 /DNDEBUG")
    SET(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /O2 /DNDEBUG")
else()
    SET(CMAKE_C_FLAGS_DEBUG "-g -Wall -D_DEBUG -DDEBUG")
    SET(CMAKE_CXX_FLAGS_DEBUG "-g -Wall -D_DEBUG -DDEBUG")
    SET(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -O3 -DNDEBUG")
    SET(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -DNDEBUG")
endif()


# 添加普通 C++ 头文件路径到包含目录
include_directories(
    ${BASE_DIR}/../deps/env/src
    ${BASE_DIR}/../deps/boringssl/src/include
    ${BASE_DIR}/../deps/jquic/include
    ${BASE_DIR}/cpp
)

if (BUILD_NODE_ADDON)
    # 设置Node.js相关变量
    set(NODE_JS_INCLUDE_DIR ${BASE_DIR}/../deps/node-v23.6.0/include/node)
    set(NODE_ADDON_API_DIR ${BASE_DIR}/../deps/node-addon-api)
    set(NODE_ADDON_SRC_DIR ${BASE_DIR}/cpp/napi)
    include_directories(
        ${NODE_ADDON_API_DIR}
        ${NODE_JS_INCLUDE_DIR}
    )

    # 如果本地node目录不存在，则使用系统缓存的node-gyp头文件
    if(NOT EXISTS ${NODE_JS_INCLUDE_DIR})
        set(NODE_JS_INCLUDE_DIR $ENV{HOME}/Library/Caches/node-gyp/23.6.0/include/node)
    endif()

    file(GLOB NODE_ADDON_SRC_FILES
        ${BASE_DIR}/cpp/*.cpp
        ${BASE_DIR}/cpp/http-parser/http_parser.c
        ${NODE_ADDON_SRC_DIR}/*.cpp
    )

    if (WIN32)
        list(APPEND NODE_ADDON_SRC_FILES
            ${BASE_DIR}/cpp/win32/win_delay_load_hook.c
        )
        add_definitions(
            -DNODE_GYP_MODULE_NAME=ttsignal
            -DUSING_UV_SHARED=1
            -DUSING_V8_SHARED=1
            -DV8_DEPRECATION_WARNINGS=1
            -D_GLIBCXX_USE_CXX11_ABI=1
            -DWIN32
            -D_CRT_SECURE_NO_DEPRECATE
            -D_CRT_NONSTDC_NO_DEPRECATE
            -D_HAS_EXCEPTIONS=0
            -DNOMINMAX
            -DNAPI_DISABLE_CPP_EXCEPTIONS
            -DBUILDING_NODE_EXTENSION
            -DHOST_BINARY="node.exe"
        )
    endif()

    # message(${NODE_ADDON_SRC_FILES})

    add_library( # Sets the name of the library.
            ${LIB_NAME}

            # Sets the library as a shared library.
            SHARED

            # Provides a relative path to your source file(s).
            ${NODE_ADDON_SRC_FILES})

    set(EXTRA_LINK_FLAGS "")

    # Node.js原生插件设置
    if (LINUX)
        message(STATUS "Building for Linux")
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|AARCH64")
            message(STATUS "Detected ARM64 architecture")
            set_target_properties(${LIB_NAME} PROPERTIES PREFIX "" SUFFIX ".linux.arm64.node")
        else()
            message(STATUS "Detected x64 architecture")
            set_target_properties(${LIB_NAME} PROPERTIES PREFIX "" SUFFIX ".linux.x64.node")
        endif()
    elseif (APPLE)
        message(STATUS "Detected OS: macOS/iOS")
        if (CMAKE_OSX_ARCHITECTURES MATCHES "arm64")
            message(STATUS "Detected ARM64 architecture")
            set_target_properties(${LIB_NAME} PROPERTIES PREFIX "" SUFFIX ".darwin.arm64.node")
        else()
            message(STATUS "Detected x64 architecture")
            set_target_properties(${LIB_NAME} PROPERTIES PREFIX "" SUFFIX ".darwin.x64.node")
        endif()
        set(EXTRA_LINK_FLAGS "-undefined dynamic_lookup")
    elseif (WIN32)
        message(STATUS "Detected OS: Windows")
        set_target_properties(${LIB_NAME} PROPERTIES PREFIX "" SUFFIX ".win32.x64.node")
        # 检查是否使用 MinGW
        if(CMAKE_C_COMPILER_ID MATCHES "GNU" OR CMAKE_CXX_COMPILER_ID MATCHES "GNU")
            # MinGW 延迟加载设置
            set(EXTRA_LINK_FLAGS "-static-libgcc -static-libstdc++ -L${BASE_DIR}/../deps/node-v23.6.0/win/x64 -lnode -lws2_32 -lkernel32 -lDbghelp -lShlwapi")
            # 添加延迟加载支持
            set(DELAYLOAD_LIBS "-ldelayimp")
        else()
            # MSVC 延迟加载设置
            set(EXTRA_LINK_FLAGS
                "${BASE_DIR}/../deps/node-v23.6.0/win/x64/node.lib"
                ws2_32.lib Dbghelp.lib Shlwapi.lib Delayimp.lib
            )
            set(MSVC_DELAYLOAD_OPTIONS "/DELAYLOAD:node.exe")
        endif()
    endif()

    # 检查是否是 GNU G++ 编译器
    if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        message(STATUS "Adding GNU/ld specific linker option: -Wl,-Bsymbolic")

        # -Wl, 是告诉编译器将后面的内容作为选项传递给链接器
        # PUBLIC/PRIVATE/INTERFACE: 决定链接选项是否会传递给依赖该目标的其他目标
        target_link_options(${LIB_NAME} PUBLIC
            "-Wl,-Bsymbolic"
        )
    endif()

    add_dependencies(${LIB_NAME} touch_utils)

    # 确保链接顺序正确
    target_link_libraries( 
        ${LIB_NAME} 
        PRIVATE
        env
        jquic
        ssl
        crypto
        ${CMAKE_DL_LIBS}
        ${EXTRA_LINK_FLAGS}
    )
    if(NOT WIN32)
        target_link_libraries(${LIB_NAME} PRIVATE pthread)
    endif()
    if(MSVC AND MSVC_DELAYLOAD_OPTIONS)
        target_link_options(${LIB_NAME} PRIVATE ${MSVC_DELAYLOAD_OPTIONS})
    endif()

    set_target_properties(${LIB_NAME} PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY_DEBUG ${BASE_DIR}/../node_modules/ttsignal/${CMAKE_BUILD_TYPE}
        LIBRARY_OUTPUT_DIRECTORY_RELEASE ${BASE_DIR}/../node_modules/ttsignal/${CMAKE_BUILD_TYPE}
        RUNTIME_OUTPUT_DIRECTORY_DEBUG ${BASE_DIR}/../node_modules/ttsignal/${CMAKE_BUILD_TYPE}
        RUNTIME_OUTPUT_DIRECTORY_RELEASE ${BASE_DIR}/../node_modules/ttsignal/${CMAKE_BUILD_TYPE}
    )
endif() # BUILD_NODE_ADDON

if (BUILD_JNI)
    # 添加 JNI 头文件路径到包含目录
    if (TARGET_OS_LINUX)
        include_directories(
            ${BASE_DIR}/../deps/openjdk/linux/include
            ${BASE_DIR}/../deps/openjdk/linux/include/linux
        )
    elseif (APPLE)
        include_directories(
            ${BASE_DIR}/../deps/openjdk/mac/include
            ${BASE_DIR}/../deps/openjdk/mac/include/darwin
        )
    endif()

    file(GLOB JNI_SRC_FILES
        ${BASE_DIR}/cpp/*.cpp
        ${BASE_DIR}/cpp/http-parser/http_parser.c
        ${BASE_DIR}/cpp/jni/*.cpp
    )

    # message(${JNI_SRC_FILES})


    add_library( # Sets the name of the library.
            "${LIB_NAME}jni"

            # Sets the library as a shared library.
            SHARED

            # Provides a relative path to your source file(s).
            ${JNI_SRC_FILES})
    
    set_target_properties(${LIB_NAME}jni PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY_DEBUG ${BASE_DIR}/../out/${CMAKE_SYSTEM_NAME}/${CMAKE_BUILD_TYPE}
        LIBRARY_OUTPUT_DIRECTORY_RELEASE ${BASE_DIR}/../out/${CMAKE_SYSTEM_NAME}/${CMAKE_BUILD_TYPE}
    )

    if (ANDROID)
        set(EXTRA_LIBS ${EXTRA_LIBS} -llog -landroid ${EXTRA_LIBS})
        set_target_properties(${LIB_NAME}jni PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY_DEBUG ${BASE_DIR}/../out/${CMAKE_SYSTEM_NAME}/${CMAKE_BUILD_TYPE}/${ANDROID_ABI}
            LIBRARY_OUTPUT_DIRECTORY_RELEASE ${BASE_DIR}/../out/${CMAKE_SYSTEM_NAME}/${CMAKE_BUILD_TYPE}/${ANDROID_ABI}
        )
    else()
        set(EXTRA_LIBS ${EXTRA_LIBS} -lpthread ${EXTRA_LIBS})
        set_target_properties(${LIB_NAME}jni PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY_DEBUG ${BASE_DIR}/../out/${CMAKE_SYSTEM_NAME}/${CMAKE_BUILD_TYPE}
            LIBRARY_OUTPUT_DIRECTORY_RELEASE ${BASE_DIR}/../out/${CMAKE_SYSTEM_NAME}/${CMAKE_BUILD_TYPE}
        )
    endif()
    add_dependencies(${LIB_NAME}jni touch_utils)

    target_link_libraries( 
        ${LIB_NAME}jni 
        PRIVATE
        env
        jquic
        ssl
        crypto
        ${EXTRA_LIBS}
        ${CMAKE_DL_LIBS}
    )

    file(GLOB JAVA_SRC_FILES
        ${BASE_DIR}/java/com/ttcall/smp/*.java
    )

    find_package(Java REQUIRED)

    include(UseJava)

    set(CMAKE_JAVA_COMPILE_FLAGS "-source" "11" "-target" "11")

    add_jar(signal
            MANIFEST ${BASE_DIR}/java/MANIFEST.MF
            ${JAVA_SRC_FILES}
            OUTPUT_DIR ${BASE_DIR}/../out/${CMAKE_SYSTEM_NAME}/${CMAKE_BUILD_TYPE})
endif()