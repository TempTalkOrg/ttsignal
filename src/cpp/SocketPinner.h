///////////////////////////////////////////////////////////////////////////////
// file   : SocketPinner.h
// author : anto
//
// 把一个 socket fd 绑定到指定网卡。本文件是整个项目里唯一知道
// IP_BOUND_IF / IPV6_BOUND_IF / SO_BINDTODEVICE / IP_UNICAST_IF /
// IPV6_UNICAST_IF / android_setsocknetwork 这些平台选项的地方。
//
// 从 UDPSender::_InitSocket 与 UDPSender::_TryClearInterfaceBinding 抽出，
// 供 UDP（QUIC）与 TCP（HTTP/WS）两条路径共用。
//
// ⚠️ TCP 的硬约束：tt_socket_pin 必须在 connect() 之前调用。
//    * macOS/iOS —— IP_BOUND_IF 对已建立连接的缓存路由无效
//    * Linux     —— TCP 的路由在 connect() 时确定；SO_BINDTODEVICE 在
//                   connect 后设置不会改变已建立的连接
//    * Windows   —— IP_UNICAST_IF 文档明确要求在 connect 前设置
///////////////////////////////////////////////////////////////////////////////

#ifndef TT_SOCKET_PINNER_H
#define TT_SOCKET_PINNER_H

#include <stdint.h>
#include <stddef.h>

#include "VpnPolicy.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TT_PIN_OK          = 0,  // 绑定成功
    TT_PIN_NOT_NEEDED  = 1,  // ifIndex / androidNetHandle 均为 0，调用方未要求绑定
    TT_PIN_UNSUPPORTED = 2,  // 当前平台无此能力
    TT_PIN_FAILED      = 3,  // 参数非法，或 setsockopt 失败（errno 见 outErrno）
} TTPinResult;

typedef struct TTPinRequest {
    int         fd;                // 待绑定的 socket
    uint32_t    ifIndex;           // 0 = 不绑定。非 Android 平台使用
    uint64_t    androidNetHandle;  // Android 用 net handle 而非 ifIndex
    int         ipv6;              // 非 0 时同时设置 IPv6 版本的选项
    int         isTcp;             // 非 0 表示 TCP。仅影响日志措辞，见下
    TTVpnPolicy policy;            // FORCE_PHYSICAL 时 Linux 优先 SO_BINDTODEVICE
    void*       loggerCtx;         // LogQ 上下文，可为 NULL
} TTPinRequest;

// 把 req->fd 绑定到 req->ifIndex 指定的网卡。
//
// outMethod     : 可为 NULL。成功时写入实际生效的手段名（"IP_BOUND_IF" /
//                 "SO_BINDTODEVICE" / "IP_UNICAST_IF" /
//                 "android_setsocknetwork"），未绑定时写入空串。
// outMethodLen  : outMethod 缓冲区大小。建议 >= 32。
// outErrno      : 可为 NULL。失败时写入 errno（Windows 为 WSAGetLastError）。
//
// isTcp 只影响一处行为：Linux 上 force-physical 时若 SO_BINDTODEVICE 失败
// （缺 CAP_NET_RAW），UDP 路径打 _WARN_ 并说明"可能拿不到真实 IP"——因为
// IP_UNICAST_IF 对已 connect 的 UDP socket 在旧内核上被静默忽略；TCP 路径
// 只打 _INFO_，因为 IP_UNICAST_IF 对 connect 前的 TCP socket 是有效的。
TTPinResult tt_socket_pin(const TTPinRequest* req,
                          char* outMethod, size_t outMethodLen,
                          int* outErrno);

// 撤销绑定，把选项设回 0。UDPSender 的 fake-IP unpin 路径使用。
//
// outMethod    : 可为 NULL。写入实际操作的选项名（"IP_BOUND_IF" /
//                "IP_UNICAST_IF"），未执行时写空串。
// outMethodLen : outMethod 缓冲区大小，建议 >= 32。
// outErrno     : 可为 NULL。写入第一个非零 errno（v4 优先，v4 成功则取 v6）；
//                全部成功时写 0。
//
// ⚠️ force-physical 下调用方不应调用本函数（既有约定，见 commit 652a603）。
//    本函数不检查策略，约束由调用方保证。
//
// Android 无公开 API 撤销 android_setsocknetwork，该平台返回 TT_PIN_UNSUPPORTED。
TTPinResult tt_socket_unpin(int fd, int ipv6, void* loggerCtx,
                            char* outMethod, size_t outMethodLen,
                            int* outErrno);

#ifdef __cplusplus
}
#endif

#endif // TT_SOCKET_PINNER_H

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
