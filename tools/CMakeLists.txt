# ---------------------------------------------------------------------------
# 命令行验证工具。由 src/CMakeLists.txt 在 -DBUILD_TOOLS=ON 时 add_subdirectory
# 进来，默认不编译（不进任何发行产物）。
#
# 刻意不去链 ttsignal 那个 .node —— 它是带 "-undefined dynamic_lookup" 的
# Node 插件，符号要等 node 进程加载时才补齐，可执行文件链不上。这里改为直接
# 编译 HTTP/WS/TCP 这条协议栈真正用到的那几个 .cpp，与
# src/cpp/tests/TcpChannel_integration_test.cpp 头部列的集合一致。
#
# SMPacket.cpp / SMPParser.cpp 是 WSConnector::SendPacket 带进来的（SMPacket 的
# PackPacket 落在 SMPParser.cpp 里），与 QUIC 那条路径无关。
# ---------------------------------------------------------------------------

set(TT_TOOLS_CORE_SRC
    ${BASE_DIR}/cpp/HttpConnector.cpp
    ${BASE_DIR}/cpp/LLHTTPParser.cpp
    ${BASE_DIR}/cpp/WSConnector.cpp
    ${BASE_DIR}/cpp/WSParser.cpp
    ${BASE_DIR}/cpp/SMPacket.cpp
    ${BASE_DIR}/cpp/SMPParser.cpp
    ${BASE_DIR}/cpp/TcpChannel.cpp
    ${BASE_DIR}/cpp/SSLayer.cpp
    ${BASE_DIR}/cpp/TlsContext.cpp
    ${BASE_DIR}/cpp/DnsResolver.cpp
    ${BASE_DIR}/cpp/SocketPinner.cpp
    ${BASE_DIR}/cpp/NetworkRouteLookup.cpp
    ${BASE_DIR}/cpp/VpnPolicy.cpp
    ${BASE_DIR}/cpp/Runtime.cpp
    ${BASE_DIR}/cpp/Utils.cpp
)

# 路径监视器 / 路由查询的平台实现。父目录在 BUILD_NODE_ADDON 分支里已经算好
# 了 TT_PATH_MONITOR_SOURCES + TT_PATH_MONITOR_LIBS 并 add_definitions 了
# TT_HAS_PATH_MONITOR；没有它的话 force-physical 会因为探测不到物理网卡而
# 直接失败（这是 TcpChannel 刻意的硬失败，不是 bug）。
if (TT_PATH_MONITOR_SOURCES)
    list(APPEND TT_TOOLS_CORE_SRC ${TT_PATH_MONITOR_SOURCES})
endif()

# httpget（HTTP 验证）与 wsecho（WebSocket 验证）用同一套源文件，只差入口。
foreach(_tt_tool httpget wsecho)
    add_executable(${_tt_tool}
        ${CMAKE_CURRENT_SOURCE_DIR}/${_tt_tool}.cpp
        ${TT_TOOLS_CORE_SRC}
    )

    target_include_directories(${_tt_tool} PRIVATE
        ${BASE_DIR}/cpp
        ${BASE_DIR}/../deps/env/src
        ${BASE_DIR}/../deps/boringssl/src/include
        ${BASE_DIR}/../deps/jquic/include
        ${BASE_DIR}/../deps/llhttp/include
    )

    target_link_libraries(${_tt_tool} PRIVATE
        env
        llhttp
        ssl
        crypto
        ${CMAKE_DL_LIBS}
    )
    if (TT_PATH_MONITOR_LIBS)
        target_link_libraries(${_tt_tool} PRIVATE ${TT_PATH_MONITOR_LIBS})
    endif()
    if (NOT WIN32)
        target_link_libraries(${_tt_tool} PRIVATE pthread)
    endif()
endforeach()

# AppleNetworkMonitor.mm 需要 ARC；同一份列表里的 AppleRouteLookup.cpp 是纯
# C++，给它加 -fobjc-arc 会触发 unused-argument 告警。
if (APPLE AND TT_PATH_MONITOR_SOURCES)
    foreach(_tt_tool_src IN LISTS TT_PATH_MONITOR_SOURCES)
        if (_tt_tool_src MATCHES "\\.mm$")
            set_source_files_properties(${_tt_tool_src}
                PROPERTIES COMPILE_FLAGS "-fobjc-arc")
        endif()
    endforeach()
endif()

set_target_properties(httpget wsecho PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
