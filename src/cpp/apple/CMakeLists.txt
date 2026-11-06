# Apple-platform sources for ttsignal.
# - AppleNetworkMonitor.{h,mm} is shared between iOS xcframework and
#   macOS NAPI builds (see src/CMakeLists.txt).
# - ios_bridge.{h,mm} + ios_logger.{h,mm} + module.modulemap are iOS
#   xcframework only (Swift binding bridge). macOS NAPI uses src/cpp/napi/
#   instead and does NOT compile these files.
#
# This file is consumed by src/CMakeLists.txt:
#   - BUILD_IOS_FRAMEWORK=ON  -> compile all five files
#   - BUILD_NODE_ADDON=ON on APPLE -> compile only AppleNetworkMonitor.mm

cmake_minimum_required(VERSION 3.20)

set(TT_APPLE_MONITOR_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/AppleNetworkMonitor.mm
    # AppleRouteLookup.cpp is plain C++ (no ObjC), so it is NOT subject
    # to the -fobjc-arc flag the parent CMakeLists applies to *.mm. It
    # is bundled into the same source list because every Apple build
    # that needs the path monitor also needs route lookups, and keeping
    # them in one variable avoids touching three call sites.
    ${CMAKE_CURRENT_SOURCE_DIR}/AppleRouteLookup.cpp
)

set(TT_APPLE_MONITOR_HEADERS
    ${CMAKE_CURRENT_SOURCE_DIR}/AppleNetworkMonitor.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../INetworkPathMonitor.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../NetworkRouteLookup.h
    # 必须导出：INetworkPathMonitor.h 会 #include 它（TTVpnPolicy 与
    # TTNetworkMonitorOptions::vpnPolicy 都要用）。漏掉它会让 xcframework
    # 的 TTSignalC module 在下游直接编不过——'VpnPolicy.h' file not found。
    ${CMAKE_CURRENT_SOURCE_DIR}/../VpnPolicy.h
)

set(TT_IOS_BRIDGE_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/ios_bridge.mm
    ${CMAKE_CURRENT_SOURCE_DIR}/ios_logger.mm
    # HTTP/WS 栈的桥接单独一个 .mm —— llhttp.h 与 http-parser/http_parser.h
    # 定义了同名的 HPE_* 枚举，并进 ios_bridge.mm 会得到一堆
    # "redefinition of enumerator 'HPE_OK'"。详见该文件顶部注释。
    ${CMAKE_CURRENT_SOURCE_DIR}/ios_http_bridge.mm
)

set(TT_IOS_BRIDGE_HEADERS
    ${CMAKE_CURRENT_SOURCE_DIR}/ios_bridge.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ios_logger.h
)

# Expose to parent scope. iOS framework target combines both lists; macOS
# NAPI target only consumes the monitor list.
set(TT_APPLE_MONITOR_SOURCES   ${TT_APPLE_MONITOR_SRC}     PARENT_SCOPE)
set(TT_APPLE_MONITOR_PUBLIC_HEADERS ${TT_APPLE_MONITOR_HEADERS} PARENT_SCOPE)

set(TT_IOS_BRIDGE_SOURCES        ${TT_IOS_BRIDGE_SRC}        PARENT_SCOPE)
set(TT_IOS_BRIDGE_PUBLIC_HEADERS ${TT_IOS_BRIDGE_HEADERS}    PARENT_SCOPE)
set(TT_IOS_BRIDGE_MODULEMAP
    ${CMAKE_CURRENT_SOURCE_DIR}/module.modulemap
    PARENT_SCOPE
)
