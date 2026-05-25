# Sets the minimum version of CMake required to build the native library.

cmake_minimum_required(VERSION 3.10)
if(POLICY CMP0048)
  cmake_policy(SET CMP0048 NEW)
endif()

project(ttsignal)

option(TARGET_PLATFORM_LINUX "Target platform is linux" OFF)
option(BUILD_JNI "Build JNI library" OFF)
option(BUILD_NODE_ADDON "Build Node.js addon" ON)
option(BUILD_IOS_FRAMEWORK "Build iOS static lib + bridge for xcframework" OFF)

if(BUILD_IOS_FRAMEWORK)
    # iOS toolchain forces these but be defensive: don't try to build the
    # node addon or JNI lib when targeting iOS.
    set(BUILD_NODE_ADDON OFF CACHE BOOL "" FORCE)
    set(BUILD_JNI        OFF CACHE BOOL "" FORCE)
endif()

if(TARGET_PLATFORM_LINUX)
    set(TARGET_OS_LINUX TRUE)
elseif(UNIX AND NOT APPLE)
    set(TARGET_OS_LINUX TRUE)
endif()

set(BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

# 把构建机绝对路径前缀替换成相对路径，避免 __FILE__ / __builtin_FILE() / DWARF
# 把开发者用户名（/Users/<name>/...）和仓库位置编进发行产物。
# 必须在 add_subdirectory(deps/...) 之前调用，directory-level COMPILE_OPTIONS
# 才会被三个 deps 子项目继承。
#
# 兼容性：
#   * -ffile-prefix-map 同时影响 __FILE__ 与 DWARF 路径，需要 GCC 8+ /
#     Clang 10+。Apple Clang (Xcode 12+) 与 Android NDK r21+ clang 都支持。
#   * 老的 GCC 7（例如部分 brew 跨编译工具链）只认 -fdebug-prefix-map，
#     这种情况下我们退而求其次只清理 DWARF 中的路径，__FILE__ 的字符串
#     仍可能残留——会打 WARNING 提示升级编译器。
#   * MSVC cl.exe：使用 /d1trimfile（VS 2019 16.4+）裁掉 __FILE__ 中的仓库根
#     前缀；同时给出反斜杠与正斜杠两种根路径，以匹配 Ninja/MSBuild 传入的源路径。
#     不对 clang-cl 添加该选项（请改用 -ffile-prefix-map）。
include(CheckCCompilerFlag)
get_filename_component(_TT_REPO_ROOT "${BASE_DIR}/.." ABSOLUTE)
if (NOT MSVC)
    check_c_compiler_flag("-ffile-prefix-map=/x=." TT_HAS_FFILE_PREFIX_MAP)
    if (TT_HAS_FFILE_PREFIX_MAP)
        add_compile_options("-ffile-prefix-map=${_TT_REPO_ROOT}=.")
    else()
        check_c_compiler_flag("-fdebug-prefix-map=/x=." TT_HAS_FDEBUG_PREFIX_MAP)
        if (TT_HAS_FDEBUG_PREFIX_MAP)
            add_compile_options("-fdebug-prefix-map=${_TT_REPO_ROOT}=.")
        endif()
        message(WARNING
            "Compiler ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION} "
            "does not support -ffile-prefix-map; absolute paths from __FILE__ "
            "may still leak into the produced binary. Consider upgrading the "
            "cross compiler to GCC 8+ / Clang 10+.")
    endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    file(TO_NATIVE_PATH "${_TT_REPO_ROOT}" _TT_MSVC_TRIM_BS)
    string(REGEX REPLACE "\\\\+$" "" _TT_MSVC_TRIM_BS "${_TT_MSVC_TRIM_BS}")
    string(APPEND _TT_MSVC_TRIM_BS "\\")
    add_compile_options(
        "$<$<COMPILE_LANGUAGE:C>:/d1trimfile:${_TT_MSVC_TRIM_BS}>"
        "$<$<COMPILE_LANGUAGE:CXX>:/d1trimfile:${_TT_MSVC_TRIM_BS}>"
    )

    file(TO_CMAKE_PATH "${_TT_REPO_ROOT}" _TT_MSVC_TRIM_FS)
    string(REGEX REPLACE "/+$" "" _TT_MSVC_TRIM_FS "${_TT_MSVC_TRIM_FS}")
    string(APPEND _TT_MSVC_TRIM_FS "/")
    add_compile_options(
        "$<$<COMPILE_LANGUAGE:C>:/d1trimfile:${_TT_MSVC_TRIM_FS}>"
        "$<$<COMPILE_LANGUAGE:CXX>:/d1trimfile:${_TT_MSVC_TRIM_FS}>"
    )
endif()

add_custom_target(touch_utils ALL
    COMMAND ${CMAKE_COMMAND} -E touch "${BASE_DIR}/cpp/Utils.cpp"
    COMMENT "Touching Utils.cpp to update build timestamp"
)

# When building the iOS framework we link against the pre-built static
# archives produced by ios/scripts/build-deps.sh — re-running the upstream
# CMake projects via add_subdirectory would (a) re-trigger the boringssl Go
# fetch every time and (b) rebuild ~5 minutes of crypto code on every core
# iteration. Always run build-deps.sh once before this CMake step.
if (NOT BUILD_IOS_FRAMEWORK)
    add_subdirectory(${BASE_DIR}/../deps/env/src deps/env.dir)
    add_subdirectory(${BASE_DIR}/../deps/boringssl/src deps/boringssl.dir)
    add_subdirectory(${BASE_DIR}/../deps/jquic deps/jquic.dir)
endif()

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

    # ----------------------------------------------------------------------
    # Cross-platform path-change monitor (INetworkPathMonitor):
    #   * Apple (macOS NAPI) -> src/cpp/apple/AppleNetworkMonitor.mm
    #   * Linux (non-Android) -> src/cpp/linux/LinuxNetlinkMonitor.cpp
    #   * Windows -> src/cpp/win32/WinIpChangeMonitor.cpp
    # All NAPI builds get TT_HAS_PATH_MONITOR so SMPConnector wires the
    # monitor up at construction. Android JNI (BUILD_JNI below) deliberately
    # does NOT define this and continues to drive Connection::Restart from
    # the Java NetworkCallback path instead.
    # ----------------------------------------------------------------------
    set(TT_PATH_MONITOR_SOURCES "")
    set(TT_PATH_MONITOR_LIBS    "")
    if (APPLE)
        add_subdirectory(${BASE_DIR}/cpp/apple ${CMAKE_BINARY_DIR}/apple-monitor.dir)
        list(APPEND TT_PATH_MONITOR_SOURCES ${TT_APPLE_MONITOR_SOURCES})
        list(APPEND TT_PATH_MONITOR_LIBS "-framework Foundation" "-framework Network")
    elseif (WIN32)
        add_subdirectory(${BASE_DIR}/cpp/win32 ${CMAKE_BINARY_DIR}/win32-monitor.dir)
        list(APPEND TT_PATH_MONITOR_SOURCES ${TT_WIN32_MONITOR_SOURCES})
        # iphlpapi.dll has NotifyIpInterfaceChange / GetBestInterfaceEx /
        # GetIfEntry2; ws2_32 covers htonl / WSAGetLastError.
        list(APPEND TT_PATH_MONITOR_LIBS iphlpapi ws2_32)
    elseif (UNIX AND NOT ANDROID)
        # Linux + any other POSIX-but-not-Android NAPI target. We still gate
        # the source itself on __linux__ inside the .cpp.
        add_subdirectory(${BASE_DIR}/cpp/linux ${CMAKE_BINARY_DIR}/linux-monitor.dir)
        list(APPEND TT_PATH_MONITOR_SOURCES ${TT_LINUX_MONITOR_SOURCES})
    endif()
    if (TT_PATH_MONITOR_SOURCES)
        list(APPEND NODE_ADDON_SRC_FILES ${TT_PATH_MONITOR_SOURCES})
        add_definitions(-DTT_HAS_PATH_MONITOR=1)
        if (APPLE)
            # AppleNetworkMonitor.mm uses ObjC dispatch + Network.framework;
            # ARC keeps reference counts sane without manual retain/release.
            set_source_files_properties(${TT_APPLE_MONITOR_SOURCES}
                PROPERTIES COMPILE_FLAGS "-fobjc-arc")
        endif()
    endif()

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
    if (TT_PATH_MONITOR_LIBS)
        target_link_libraries(${LIB_NAME} PRIVATE ${TT_PATH_MONITOR_LIBS})
    endif()
    if(NOT WIN32)
        target_link_libraries(${LIB_NAME} PRIVATE pthread)
    endif()
    if(MSVC AND MSVC_DELAYLOAD_OPTIONS)
        target_link_options(${LIB_NAME} PRIVATE ${MSVC_DELAYLOAD_OPTIONS})
    endif()

    set_target_properties(${LIB_NAME} PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY_DEBUG ${BASE_DIR}/../node_modules/ttsignal/dist/build/${CMAKE_BUILD_TYPE}
        LIBRARY_OUTPUT_DIRECTORY_RELEASE ${BASE_DIR}/../node_modules/ttsignal/dist/build/${CMAKE_BUILD_TYPE}
        RUNTIME_OUTPUT_DIRECTORY_DEBUG ${BASE_DIR}/../node_modules/ttsignal/dist/build/${CMAKE_BUILD_TYPE}
        RUNTIME_OUTPUT_DIRECTORY_RELEASE ${BASE_DIR}/../node_modules/ttsignal/dist/build/${CMAKE_BUILD_TYPE}
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

if (BUILD_IOS_FRAMEWORK)
    # Pull in the Apple monitor + iOS Swift bridge sources / public headers /
    # module map (TT_APPLE_MONITOR_* + TT_IOS_BRIDGE_*).
    add_subdirectory(${BASE_DIR}/cpp/apple ${CMAKE_BINARY_DIR}/apple.dir)

    # Caller (build-core.sh) is expected to pass IOS_DEPS_DIR pointing at
    # build/ios-deps/<sdk>-<arch>/lib, where build-deps.sh has dropped the
    # pre-built libssl / libcrypto / libjquic / libenv archives. If not
    # supplied, fall back to the conventional layout.
    if (NOT IOS_DEPS_DIR)
        # Reconstruct from CMAKE_OSX_SYSROOT + CMAKE_SYSTEM_PROCESSOR.
        get_filename_component(_sdk_basename "${CMAKE_OSX_SYSROOT}" NAME)
        # SDK path looks like .../iPhoneOS18.5.sdk — strip version + .sdk to get sdk name
        string(REGEX REPLACE "([A-Za-z]+)[0-9.]*\\.sdk$" "\\1" _sdk_name "${_sdk_basename}")
        string(TOLOWER "${_sdk_name}" _sdk_name_lc)
        set(IOS_DEPS_DIR "${BASE_DIR}/../build/ios-deps/${_sdk_name_lc}-${CMAKE_SYSTEM_PROCESSOR}/lib")
    endif()
    if (NOT EXISTS "${IOS_DEPS_DIR}/libssl.a")
        message(FATAL_ERROR
            "BUILD_IOS_FRAMEWORK requires pre-built deps. Expected libssl.a "
            "under ${IOS_DEPS_DIR}. Run ios/scripts/build-deps.sh first.")
    endif()
    message(STATUS "ttsignal_ios: deps from ${IOS_DEPS_DIR}")

    file(GLOB TT_IOS_CORE_SRC
        ${BASE_DIR}/cpp/*.cpp
        ${BASE_DIR}/cpp/http-parser/http_parser.c
    )

    add_library(ttsignal_ios STATIC
        ${TT_IOS_CORE_SRC}
        ${TT_APPLE_MONITOR_SOURCES}
        ${TT_IOS_BRIDGE_SOURCES}
    )
    add_dependencies(ttsignal_ios touch_utils)

    # Tell SMPConnector to link against tt_netmon_* — same flag used by
    # macOS / Linux / Windows NAPI builds so the runtime path is identical.
    target_compile_definitions(ttsignal_ios PRIVATE TT_HAS_PATH_MONITOR=1)

    target_include_directories(ttsignal_ios PUBLIC
        ${BASE_DIR}/cpp
        ${BASE_DIR}/cpp/apple
        ${BASE_DIR}/../deps/env/src
        ${BASE_DIR}/../deps/boringssl/src/include
        ${BASE_DIR}/../deps/jquic/include
    )

    # ARC for the .mm bridge files. Plain .cpp/.c files get compiled the
    # normal way (no ObjC runtime).
    set_source_files_properties(
        ${TT_APPLE_MONITOR_SOURCES}
        ${TT_IOS_BRIDGE_SOURCES}
        PROPERTIES COMPILE_FLAGS "-fobjc-arc")

    # Link against pre-built static archives.
    target_link_libraries(ttsignal_ios PRIVATE
        ${IOS_DEPS_DIR}/libenv.a
        ${IOS_DEPS_DIR}/libjquic.a
        ${IOS_DEPS_DIR}/libssl.a
        ${IOS_DEPS_DIR}/libcrypto.a
    )

    # Apple system frameworks needed by AppleNetworkMonitor / ios_logger.
    target_link_libraries(ttsignal_ios PRIVATE
        "-framework Foundation"
        "-framework Network"
    )

    # Position-independent code & C++ standard floor for the iOS build.
    set_target_properties(ttsignal_ios PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        OUTPUT_NAME "ttsignal_ios"
        ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib
    )

    # Stage the public headers + module.modulemap next to the .a so the
    # ios/scripts/build-xcframework.sh helper can pick them up without
    # having to reach into src/cpp/apple/ across slices.
    set(_TT_IOS_STAGE ${CMAKE_BINARY_DIR}/stage)
    file(MAKE_DIRECTORY ${_TT_IOS_STAGE}/Headers ${_TT_IOS_STAGE}/Modules)
    foreach(_h ${TT_APPLE_MONITOR_PUBLIC_HEADERS} ${TT_IOS_BRIDGE_PUBLIC_HEADERS})
        get_filename_component(_hn ${_h} NAME)
        configure_file(${_h} ${_TT_IOS_STAGE}/Headers/${_hn} COPYONLY)
    endforeach()
    configure_file(${TT_IOS_BRIDGE_MODULEMAP}
                   ${_TT_IOS_STAGE}/Modules/module.modulemap COPYONLY)

    message(STATUS "ttsignal: iOS framework bits staged at ${_TT_IOS_STAGE}")
endif() # BUILD_IOS_FRAMEWORK