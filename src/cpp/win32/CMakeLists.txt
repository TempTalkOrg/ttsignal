# Windows-only network path monitor (NotifyIpInterfaceChange + GetBestInterfaceEx).
# Pulled in by src/CMakeLists.txt only when BUILD_NODE_ADDON=ON on Windows.

cmake_minimum_required(VERSION 3.10)

set(TT_WIN32_MONITOR_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/WinIpChangeMonitor.cpp
)

set(TT_WIN32_MONITOR_HEADERS
    ${CMAKE_CURRENT_SOURCE_DIR}/WinIpChangeMonitor.h
)

set(TT_WIN32_MONITOR_SOURCES        ${TT_WIN32_MONITOR_SRC}     PARENT_SCOPE)
set(TT_WIN32_MONITOR_PUBLIC_HEADERS ${TT_WIN32_MONITOR_HEADERS} PARENT_SCOPE)
