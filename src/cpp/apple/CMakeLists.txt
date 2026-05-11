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
)

set(TT_APPLE_MONITOR_HEADERS
    ${CMAKE_CURRENT_SOURCE_DIR}/AppleNetworkMonitor.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../INetworkPathMonitor.h
)

set(TT_IOS_BRIDGE_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/ios_bridge.mm
    ${CMAKE_CURRENT_SOURCE_DIR}/ios_logger.mm
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
