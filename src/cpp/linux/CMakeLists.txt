# Linux-only network path monitor (NETLINK_ROUTE).
# Pulled in by src/CMakeLists.txt only when BUILD_NODE_ADDON=ON on a
# non-Android Linux target.

cmake_minimum_required(VERSION 3.10)

set(TT_LINUX_MONITOR_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/LinuxNetlinkMonitor.cpp
)

set(TT_LINUX_MONITOR_HEADERS
    ${CMAKE_CURRENT_SOURCE_DIR}/LinuxNetlinkMonitor.h
)

set(TT_LINUX_MONITOR_SOURCES        ${TT_LINUX_MONITOR_SRC}     PARENT_SCOPE)
set(TT_LINUX_MONITOR_PUBLIC_HEADERS ${TT_LINUX_MONITOR_HEADERS} PARENT_SCOPE)
