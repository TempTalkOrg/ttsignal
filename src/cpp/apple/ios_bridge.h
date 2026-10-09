///////////////////////////////////////////////////////////////////////////////
// file : ios_bridge.h
// author : anto
//
// extern "C" API exposed to Swift via TTSignalC module (module.modulemap).
// Mirror of src/cpp/jni/JNI_SMPConnectorWrap.cpp — every method on
// Connector / Connection / Packet has an equivalent here, kept 1:1 so the
// Swift binding layer in src/swift/ matches the Java binding in src/java/.
///////////////////////////////////////////////////////////////////////////////
#ifndef TTSIGNAL_IOS_BRIDGE_H_INCLUDED__
#define TTSIGNAL_IOS_BRIDGE_H_INCLUDED__

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

///////////////////////////////////////////////////////////////////////////////
// Opaque handles
///////////////////////////////////////////////////////////////////////////////

typedef struct TTConnector*  TTConnectorRef;
typedef struct TTConnection* TTConnectionRef;
typedef struct TTPacket*     TTPacketRef;

///////////////////////////////////////////////////////////////////////////////
// Config — flat C struct, populated by Swift TTSignalConfig before
// tt_connector_create / tt_connector_create_connection. Mirrors
// src/java/.../Config.java field-by-field.
///////////////////////////////////////////////////////////////////////////////

typedef struct {
    // Connector use
    const char* hostname;
    // Server use
    int32_t     port;
    int32_t     backlog;
    int32_t     reusePort;     // bool
    int32_t     ssl;           // bool
    const char* privateKeyFile;
    const char* certificateFile;
    // Server / Connector both
    int32_t     taskThreads;
    int32_t     timerThreads;
    int32_t     idleTimeOut;
    const char* alpn;
    int32_t     maxConnections;
    int32_t     congestCtrl;   // 'B' / 'b' / 'C' / 'R' ...
    int32_t     pingOn;        // bool
    int32_t     pingInterval;
    int32_t     activeConnectionIdLimit;
    int32_t     deviceType;
    const char* cidTag;
    const char* logFile;
    int32_t     logLevel;
    int32_t     numOfSenders;
    const char* serverHost;
    const char* caCertPem;
    // Outbound MASQUE proxy (RFC 9298 CONNECT-UDP). When set, connections
    // tunnel their QUIC traffic through the proxy instead of dialing the
    // target directly. proxyUrl is the primary input; proxyHost / proxyPort /
    // proxySni override the parsed values. NULL/empty/0 = direct (no proxy).
    // The bridge only forwards these to BCFObject when non-empty, since the
    // native Config treats a present proxy_host (even empty) or a non-zero
    // proxy_port as "enable proxy" — see ConvertConfig in ios_bridge.mm.
    const char* proxyUrl;
    const char* proxyHost;
    int32_t     proxyPort;   // 0 = unset (defaults to 443 when proxy enabled)
    const char* proxySni;
    // Self-signed root CA (PEM) used to verify the outer hop to the proxy.
    // NULL/empty = use the system trust store for the proxy's TLS cert.
    const char* proxyCaCertPem;
    // Base64 SHA-256 SPKI pin for the proxy's leaf certificate. When set, the
    // outer CONNECT-UDP hop is pinned to this public key. NULL/empty = no pin.
    const char* spkiPin;
    // Off-switch for SMPConnector's built-in auto-restart on path changes.
    // 0 (default) keeps the AppleNetworkMonitor → SMPConnection::Restart
    // pipeline live, which is what apps want — they get free QUIC
    // connection migration across cellular ↔ wifi without writing a
    // single line of code. Set to 1 for server / long-lived deployments
    // (mirrors the NAPI `config.disableAutoRestart`) where you don't
    // want NWPathMonitor jitter to bounce well-behaved connections.
    int32_t     disableAutoRestart;
    // Tri-state: -1 (default) keeps the platform default, 0 forces "let
    // QUIC ride VPN/utun tunnels when the OS prefers them", 1 forces
    // "always prefer wifi/wired/cellular over a tunnel". macOS is the
    // only platform that consults this; iOS / Linux / Windows monitors
    // accept the value for API parity but ignore it. See
    // TTNetworkMonitorOptions::bypassVpn in INetworkPathMonitor.h.
    // We use -1 as the sentinel (rather than just "0 = unset") so that
    // C-struct zero-initialised callers still get the safe default, and
    // an explicit caller can pick either side.
    int32_t     bypassVpn;
    // 三态 VPN / 虚拟网卡策略，取代已废弃的 bypassVpn。
    //   -1 未设置（用平台默认值：iOS 是 os，桌面是 prefer-physical）
    //    0 os              — 跟随系统路由，允许走 VPN。iOS 的默认与历史行为
    //    1 prefer-physical — 优先物理网卡，运行中拒绝回落隧道
    //    2 force-physical  — 只走物理网卡；找不到就让 connect 立即失败并
    //                        回调 BC_R_NO_PHYSICAL_INTERFACE (64)
    //
    // 与 bypassVpn 同时设置时本字段胜出（原生层会打一条 WARN）。沿用 -1 作
    // unset 哨兵而不是 0，这样 C-struct 零初始化的调用方拿到的是"未设置"
    // 而不是"os"。取值定义见 src/cpp/VpnPolicy.h 的 TTVpnPolicy。
    int32_t     vpnPolicy;
} TTConfig;

///////////////////////////////////////////////////////////////////////////////
// Handler vtable — every entry maps 1:1 to IConnectionHandler.java
///////////////////////////////////////////////////////////////////////////////

typedef struct {
    void (*on_connect_result)(void* userdata,
                              int32_t error,
                              const char* message);
    void (*on_stream_created)(void* userdata,
                              int32_t streamId);
    void (*on_stream_closed)(void* userdata,
                             int32_t streamId);
    void (*on_stream_data_acked)(void* userdata,
                                 int32_t streamId,
                                 int64_t ackDelayUs,
                                 int32_t ackedBytes,
                                 int32_t inflightBytes);
    void (*on_stream_data_sent)(void* userdata,
                                int32_t streamId,
                                int32_t transId,
                                int32_t size);
    void (*on_recv_cmd)(void* userdata,
                        int64_t timestamp,
                        int32_t transId,
                        int32_t streamId,
                        const uint8_t* data,
                        size_t len);
    void (*on_recv_data)(void* userdata,
                         int64_t timestamp,
                         int32_t transId,
                         int32_t streamId,
                         const uint8_t* data,
                         size_t len);
    void (*on_restart)(void* userdata,
                       int32_t result,
                       const char* localAddr);
    void (*on_closed)(void* userdata,
                      const char* reason);
    void (*on_exception)(void* userdata,
                         const char* errMsg);
} TTHandlerVTable;

///////////////////////////////////////////////////////////////////////////////
// Connector — see JNI_SMPConnectorWrap::SMPConnectorWrap
///////////////////////////////////////////////////////////////////////////////

// Returns NULL on failure. Caller owns the returned handle and must call
// tt_connector_destroy() once the underlying SMPConnector has fired
// OnClosed().
TTConnectorRef tt_connector_create(const TTConfig* config);

// Spawn a Connection on the engine-shared Connector. config may be the same
// as the connector config or a per-connection override (e.g. different alpn /
// idle_time_out). handler vtable callbacks are dispatched on internal worker
// threads — Swift bindings must hop to the main queue if needed. userdata is
// returned verbatim to every callback and is opaque to the bridge.
TTConnectionRef tt_connector_create_connection(TTConnectorRef connector,
                                               const TTConfig* config,
                                               const TTHandlerVTable* vtable,
                                               void* userdata);

// outBuf receives a JSON object string with keys mirroring
// SMPConnector::GetStats() (allocated_conn_size / active_conn_size /
// freed_conn_size). bufSize must be >= 256. Returns the number of bytes
// written (excluding NUL); 0 on error.
size_t tt_connector_get_stats(TTConnectorRef connector,
                              char* outBuf,
                              size_t bufSize);

// Initiate connector shutdown. The handler's on_closed will be fired
// asynchronously when the underlying engine has actually stopped.
void tt_connector_close(TTConnectorRef connector);

// Synchronous variant of tt_connector_close: kick shutdown and block
// the calling thread until the C++ engine has fully drained (i.e. the
// IConnectorHandler::OnClosed callback has fired) or `timeoutMs` ms
// have elapsed. Returns 0 (BC_R_SUCCESS) on clean close, 2
// (BC_R_TIMEDOUT) on timeout, or another BC_R_* code if the handle is
// invalid. Pass `timeoutMs <= 0` to wait forever.
//
// Use this from any teardown path that immediately wants to call
// tt_connector_destroy or rebuild a fresh connector (e.g. switching
// log level at runtime, or app shutdown that needs to free resources
// before exit). The plain async tt_connector_close otherwise races
// the destroy and can crash worker threads still touching SMPConnector
// after delete.
//
// MUST NOT be called from inside a TTHandlerVTable callback — the
// OnClosed wake-up runs on the same internal worker pool, so blocking
// it would deadlock.
int32_t tt_connector_close_sync(TTConnectorRef connector, int32_t timeoutMs);

// Final delete — only call after on_closed fired (or if create failed).
void tt_connector_destroy(TTConnectorRef connector);

///////////////////////////////////////////////////////////////////////////////
// Connection — see JNI_SMPConnectorWrap::SMPConnectionWrap
///////////////////////////////////////////////////////////////////////////////

// Initiate a QUIC connection to `url` (full URL, e.g. https://host:443/path).
// `propsJson` is a JSON object string pinned by the caller — it is forwarded
// verbatim to the server inside the SMP handshake (matches JNI's `props`).
// Returns BC_R_SUCCESS (0) on success or one of the BC_R_* error codes.
int32_t tt_connection_connect(TTConnectionRef connection,
                              const char* url,
                              const char* propsJson,
                              int32_t timeoutMs);

// Send a packet that was previously built via tt_packet_create. The bridge
// retains a reference to the underlying SMPacket until the engine has flushed
// it — the caller may destroy the TTPacketRef immediately after this returns.
int32_t tt_connection_send_packet(TTConnectionRef connection,
                                  TTPacketRef packet);

// Trigger an active QUIC migration. networkHandle == 0 keeps the existing
// interface (matches Android's restart()). On iOS networkHandle is the
// numeric ifIndex returned by nw_interface_get_index, fed straight through
// SMPConnection::Restart -> UDPSender::Restart -> setsockopt(IP_BOUND_IF).
//
// Application code does NOT normally call this — AppleNetworkMonitor invokes
// it automatically when NWPathMonitor reports a usable path change.
void tt_connection_restart(TTConnectionRef connection, int64_t networkHandle);

void tt_connection_close(TTConnectionRef connection);
void tt_connection_close_stream(TTConnectionRef connection, int32_t streamId);
void tt_connection_destroy(TTConnectionRef connection);

///////////////////////////////////////////////////////////////////////////////
// Packet — see JNI_SMPacketWrap
///////////////////////////////////////////////////////////////////////////////

// type matches Const.PTYPE_* in src/java/.../Const.java
//   PTYPE_CMD          = 1
//   PTYPE_DATA         = 2
//   PTYPE_USER_CONTROL = 3
//   PTYPE_PING         = 4
//   PTYPE_PONG         = 5
// data is copied internally — the caller can free its buffer immediately.
TTPacketRef tt_packet_create(uint8_t  type,
                             int64_t  timestamp,
                             int32_t  transId,
                             int32_t  streamId,
                             const uint8_t* data,
                             size_t   len);

void tt_packet_destroy(TTPacketRef packet);

///////////////////////////////////////////////////////////////////////////////
// Versioning — useful for sanity-checking xcframework was built from the
// same revision as the Swift binding it ships with.
///////////////////////////////////////////////////////////////////////////////

const char* tt_get_sdk_version(void);

///////////////////////////////////////////////////////////////////////////////
// HTTP / WebSocket —— 真实 IP 出网的 TCP/TLS 栈
//
// 与上面 SMP/QUIC 那套是**两条独立的栈**，不共用 connector。这条走 TCP，用途是
// 在 VPN / 虚拟网卡环境下用**用户真实 IP** 出网（vpnPolicy = force-physical），
// 典型场景是请求接入点接口 —— 走 VPN 出口拿到的接入点会被调度到错误地域。
//
// 通用约定（与上面的 TTHandlerVTable 一致）：
//   * 回调参数里的指针**只在回调期间有效**，调用方必须立刻拷贝；
//   * 回调发生在工作线程 / 连接的事件循环线程，**都不是主线程**；
//   * int32_t 错误码是 BCRESULT，与 Node 侧同一套跨语言契约值：
//       64 BC_R_NO_PHYSICAL_INTERFACE  当下没有可用物理网卡
//       69 BC_R_PIN_FAILED             绑定网卡失败
//       70 BC_R_ROUTE_MISMATCH         对端从物理网卡出不去
//       71 BC_R_DNS_FAILED             DNS 失败
//       72 BC_R_TLS_VERIFY_FAILED      证书校验失败
//       73 BC_R_RESPONSE_TOO_LARGE     响应超过 maxResponseBytes（仅 HTTP）
//       74 BC_R_WS_HANDSHAKE_FAILED    WebSocket 握手失败（仅 WS）
///////////////////////////////////////////////////////////////////////////////

typedef struct TTHttpConnector* TTHttpConnectorRef;

// 连接器关闭完成的信号。HTTP 与 WS 共用。
typedef void (*TTCloseDone)(void* userdata);

typedef struct {
    // -1 = 平台默认（不写这个键，交给原生层决定）。取值见 VpnPolicy.h 的
    // TTVpnPolicy —— 与 TTConfig.vpnPolicy 同一套。
    int32_t     vpnPolicy;
    const char* caCerts;            // 额外信任的 PEM，NULL/"" = 不加
    const char* spkiPin;
    int32_t     insecureSkipVerify; // 非 0 = 跳过证书校验（别在生产用）
    const char* dnsServers;
    uint32_t    dnsTimeoutMs;
    uint32_t    drainTimeoutMs;     // 关闭时等在途请求的上限
    int32_t     logLevel;           // 1 DEBUG ... 5 FATAL，0 = 用默认
    size_t      maxResponseBytes;   // 0 = 用默认（8MiB）
} TTHttpConfig;

typedef struct {
    const char* method;             // NULL => "GET"
    const char* url;                // 必填，http:// 或 https://
    const char* const* headerNames; // 与 headerValues 平行，长度 headerCount
    const char* const* headerValues;
    size_t      headerCount;
    const uint8_t* body;
    size_t      bodyLen;
    uint32_t    timeoutMs;          // 0 = 用默认。覆盖 DNS+connect+TLS+收响应
    const char* resolvedIp;         // 非 NULL 时跳过 DNS，直接连这个 IP
} TTHttpRequest;

typedef struct {
    int32_t     status;
    // status line 的 reason-phrase。⚠️ **原始网络字节，未做净化** —— 可能含
    // 控制字符（包括 ESC，足以往终端注入 ANSI 转义序列）。打到终端/日志前自己过滤。
    const char* reason;
    // 头**名统一小写**；同名头已按 RFC 7230 3.2.2 以 ", " 合并 —— 这会拼坏
    // Set-Cookie（那是该规则的著名例外）。本栈定位是 JSON API 客户端，需要完整
    // Set-Cookie 的调用方不能用这里的 headers。
    const char* const* headerNames;
    const char* const* headerValues;
    size_t      headerCount;
    const uint8_t* body;
    size_t      bodyLen;
    // 以下三项让业务无需翻日志就能判断这次请求有没有真的走物理网卡
    const char* peerIp;
    uint32_t    boundIfIndex;       // 非 0 = 真的绑上了物理网卡
    const char* pinMethod;          // "IP_BOUND_IF" / "SO_BINDTODEVICE" / ""
} TTHttpResponse;

// error == 0 时 resp 非 NULL；error != 0 时 resp 为 NULL，看 errName/errMessage。
// 4xx/5xx 算**正常响应**（error == 0），与 Node 侧一致。
// errMessage 是原生层的现场描述 —— force-physical 失败时那段写得很细，是排查
// 的唯一依据，别吞掉也别改写。
// 契约：每次 tt_http_request 成功受理后，本回调**恰好触发一次**。
typedef void (*TTHttpCompletion)(void* userdata,
                                 int32_t error,
                                 const char* errName,
                                 const char* errMessage,
                                 const TTHttpResponse* resp);

// BCRESULT 的符号名，如 "BC_R_ROUTE_MISMATCH"。表外的码给 "BC_R_(<数值>)"。
// 返回的是静态存储（每线程一份），调用方不要 free。
//
// 为什么需要它：tt_http_request / tt_ws_connection_connect 同步返回错误码时
// 没有 completion 可以带 errName，绑定层只能自己查表 —— 而错误码名表是
// src/cpp/Utils.h 里那一份共用的，不该在 Swift 侧再抄一遍。
const char* tt_result_name(int32_t result);

// 最近一次**同步失败**的现场描述；没有则返回空串。返回的是静态存储
// （每线程一份），调用方不要 free。
//
// 取值时机：在同一条线程上、紧接着返回非 0 的那次 tt_http_request /
// tt_ws_connection_connect 调用之后立刻取。下一次同步调用会覆盖它。
//
// 为什么需要它：URL 解析失败、请求组装失败、force-physical 拿不到物理网卡
// （BC_R_NO_PHYSICAL_INTERFACE）都是同步失败 —— 走不到 on_connect_result /
// completion，文案原本只落在库日志里，业务只看得到一个错误码。
const char* tt_last_sync_error(void);

// 返回 NULL 表示创建失败。用完必须 tt_http_connector_destroy。
TTHttpConnectorRef tt_http_connector_create(const TTHttpConfig* config);
// 返回值只表示"是否成功受理"。**非 0 时 completion 不会被调用**，调用方需要
// 自己回收 userdata，否则泄漏。
int32_t tt_http_request(TTHttpConnectorRef connector,
                        const TTHttpRequest* req,
                        TTHttpCompletion completion,
                        void* userdata);
// done 在在途请求收尾后触发。可重复调用。
void tt_http_connector_close(TTHttpConnectorRef connector,
                             TTCloseDone done, void* userdata);
void tt_http_connector_destroy(TTHttpConnectorRef connector);

///////////////////////////////////////////////////////////////////////////////
// WebSocket —— 长连接
///////////////////////////////////////////////////////////////////////////////

typedef struct TTWSConnector*  TTWSConnectorRef;
typedef struct TTWSConnection* TTWSConnectionRef;

typedef struct {
    // --- 与 TTHttpConfig 前 8 项同义 ---
    int32_t     vpnPolicy;          // -1 = 平台默认
    const char* caCerts;
    const char* spkiPin;
    int32_t     insecureSkipVerify;
    const char* dnsServers;
    uint32_t    dnsTimeoutMs;
    uint32_t    drainTimeoutMs;
    int32_t     logLevel;
    // --- WS 专有，0 / NULL 一律用原生默认 ---
    uint32_t    connectTimeoutMs;
    uint32_t    pingIntervalMs;
    uint32_t    idleTimeoutMs;
    size_t      maxFrameBytes;
    const char* clientCertFile;
    const char* clientKeyFile;
    const char* clientKeyPassword;
} TTWSConfig;

///////////////////////////////////////////////////////////////////////////////
// 事件回调。全部发生在该连接的事件循环线程上，**不是主线程**。
//
// 生命周期（原文见 WSConnector.h 顶部的契约 1-3）：
//   * tt_ws_connection_connect 返回非 0 时，一个回调都不会有；
//   * 返回 0 之后 on_connect_result 恰好一次；
//       - error == 0  => 之后还会恰好一次 on_closed；
//       - error != 0  => **不再有 on_closed**（握手没成功，也就没有"连接关闭"）。
//
// on_release 是给绑定层收尾用的：bridge 依据上面那套契约判断"不会再有任何
// 回调了"，然后调它**恰好一次**。Swift 侧只需在这里释放自己的 box，不必自己
// 推理该不该释放 —— 推错的后果是泄漏或 use-after-free。
///////////////////////////////////////////////////////////////////////////////

typedef struct {
    void (*on_connect_result)(void* ud, int32_t error, const char* message);
    void (*on_text)     (void* ud, const char* text, size_t len);
    void (*on_data)     (void* ud, const uint8_t* data, size_t len);
    void (*on_closed)   (void* ud, const char* reason);
    void (*on_exception)(void* ud, const char* errMsg);
    void (*on_release)  (void* ud);
} TTWSHandlerVTable;

TTWSConnectorRef tt_ws_connector_create(const TTWSConfig* config);
// 返回 NULL 表示失败。返回非 NULL 时 vtable 已被拷贝，调用方不必保留它。
TTWSConnectionRef tt_ws_connector_create_connection(TTWSConnectorRef connector,
                        const TTWSHandlerVTable* vtable, void* userdata);
void tt_ws_connector_close(TTWSConnectorRef connector,
                           TTCloseDone done, void* userdata);
void tt_ws_connector_destroy(TTWSConnectorRef connector);

// url 形如 ws://host[:port]/path 或 wss://...，host 可以是域名。
// timeoutMs 为 0 时用配置里的 connectTimeoutMs。
int32_t tt_ws_connection_connect(TTWSConnectionRef conn,
                                 const char* url, uint32_t timeoutMs);
// 必须在 connect 之前调用，之后调用返回 BC_R_ALREADYRUNNING。
// Host / Upgrade / Connection / Sec-WebSocket-* 由原生层权威决定，同名项会被忽略。
int32_t tt_ws_connection_set_request_header(TTWSConnectionRef conn,
                                            const char* name, const char* value);
// ⚠️ 不校验 text 是否为合法 UTF-8（RFC 6455 5.6 要求文本帧必须是），
// 需要严格性的调用方自己把关。
int32_t tt_ws_connection_send_text(TTWSConnectionRef conn,
                                   const char* text, size_t len);
int32_t tt_ws_connection_send_data(TTWSConnectionRef conn,
                                   const uint8_t* data, size_t len);
int32_t tt_ws_connection_send_ping(TTWSConnectionRef conn);
// 非 0 = 真的绑上了物理网卡。握手成功后才有意义。
uint32_t tt_ws_connection_bound_if(TTWSConnectionRef conn);
void tt_ws_connection_close(TTWSConnectionRef conn);
void tt_ws_connection_destroy(TTWSConnectionRef conn);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // TTSIGNAL_IOS_BRIDGE_H_INCLUDED__
