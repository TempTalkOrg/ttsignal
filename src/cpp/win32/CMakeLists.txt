# Windows-only network path monitor (NotifyIpInterfaceChange + GetBestInterfaceEx).
# Pulled in by src/CMakeLists.txt only when BUILD_NODE_ADDON=ON on Windows.

cmake_minimum_required(VERSION 3.10)

set(TT_WIN32_MONITOR_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/WinIpChangeMonitor.cpp
    # Kernel route-table query helper. Implements tt_route_lookup_ifindex
    # declared in src/cpp/NetworkRouteLookup.h via iphlpapi's
    # GetBestInterfaceEx; UDPSender uses it to validate IP_UNICAST_IF
    # hints on Windows.
    ${CMAKE_CURRENT_SOURCE_DIR}/WinRouteLookup.cpp
)

set(TT_WIN32_MONITOR_HEADERS
    ${CMAKE_CURRENT_SOURCE_DIR}/WinIpChangeMonitor.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../NetworkRouteLookup.h
)

set(TT_WIN32_MONITOR_SOURCES        ${TT_WIN32_MONITOR_SRC}     PARENT_SCOPE)
set(TT_WIN32_MONITOR_PUBLIC_HEADERS ${TT_WIN32_MONITOR_HEADERS} PARENT_SCOPE)
