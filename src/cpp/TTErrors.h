///////////////////////////////////////////////////////////////////////////////
// file   : TTErrors.h
// author : anto
//
// ttsignal 自定义的 BCRESULT 错误码，统一以 BC_R_NRESULTS + N 编号。
//
// 在此之前这些码散落在各 .cpp 里各自 #define（WSConnector.cpp 和
// WSServer.cpp 就各定义了一遍 BC_R_IDLE_TIMEOUT），集中到这里避免重复与
// 取值冲突。新增错误码请依次往下排，不要复用已有编号。
///////////////////////////////////////////////////////////////////////////////

#ifndef TT_ERRORS_H
#define TT_ERRORS_H

#include <BC/Config.h>

// 沿用既有取值，原先定义在 WSConnector.cpp:32-33
#define BC_R_IDLE_TIMEOUT               (BC_R_NRESULTS + 1)
#define BC_R_CONNECT_TIMEOUT            (BC_R_NRESULTS + 2)

// BC_R_NO_PHYSICAL_INTERFACE 不在这里定义 —— deps/env/src/BC/Config.h:388
// 已经把它定成 64，而 64 是写进 src/swift/TTSignalConfig.swift 和
// src/js/index.js 文档的跨语言契约值，不能改。BC_R_NRESULTS + 3 这个槽位
// 因此空置，不要拿它去定义别的码，避免将来有人对着编号连续性犯同样的错。

// force-physical 下 tt_socket_pin 失败
#define BC_R_PIN_FAILED                 (BC_R_NRESULTS + 4)
// 路由复核发现内核仍会把包送进隧道
#define BC_R_ROUTE_MISMATCH             (BC_R_NRESULTS + 5)
// 所有 DNS server 均失败
#define BC_R_DNS_FAILED                 (BC_R_NRESULTS + 6)
// 证书链 / 主机名 / SPKI pin 校验失败
#define BC_R_TLS_VERIFY_FAILED          (BC_R_NRESULTS + 7)
// 响应体超过 maxResponseBytes
#define BC_R_RESPONSE_TOO_LARGE         (BC_R_NRESULTS + 8)
// WebSocket 握手失败：响应不是 101，或 Upgrade / Connection /
// Sec-WebSocket-Accept 校验不通过。具体原因在 OnConnectResult 之前打的日志里。
#define BC_R_WS_HANDSHAKE_FAILED        (BC_R_NRESULTS + 9)

#endif // TT_ERRORS_H

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
