# iOS HTTP/WS binding：ios_bridge C-ABI 导出与 Swift 封装

日期：2026-09-07
状态：设计已确认，待实现

## 背景与问题

`HttpConnector` / `WSConnector` 这套 TCP/TLS/HTTP/WS 客户端栈（见 [2026-08-17-tcp-http-ws-stack-design.md](2026-08-17-tcp-http-ws-stack-design.md)）目前只有 **Node NAPI** 一个绑定。iOS 侧的状况是：

- 2026-09-07 之前：`src/CMakeLists.txt` 用 `list(REMOVE_ITEM ...)` 把 `HttpConnector.cpp` / `LLHTTPParser.cpp` / `WSConnector.cpp` / `WSParser.cpp` 四个文件排除在 `ttsignal_ios` 之外，因为 `build/ios-deps/<slice>/lib/` 下没有 `libllhttp.a`，不排除就会在链接期报一串 `llhttp_*` undefined symbol。
- 同日已补齐 llhttp：`ios/scripts/build-deps.sh` 编出三 slice 的 `libllhttp.a`，`build-xcframework.sh`（三处）与 `build-quictest.sh`（一处）的合并列表都加上了它，`REMOVE_ITEM` 已删除。验证结果：xcframework 的 device 与 simulator slice 均含 `WS::WSConnection` 127 个、`llhttp_` 98 个、`HttpConnector` 27 个符号。

**当前缺口**：代码进了产物，但 Swift 侧调不到。`libTTSignal.a` 对外导出的 30 个 `_tt_*` C-ABI 全是 SMP/QUIC 系（`tt_connector_*` / `tt_connection_*` / `tt_packet_*` / `tt_netmon_*` / `tt_vpn_*` / `tt_route_*`），没有任何 HTTP/WS 入口；`ios_bridge.mm` 里出现的 `http` 只是 URL 解析用的 `http_parser`，`src/swift/` 七个文件里也只有一行文档注释提到 https。

本设计要补的就是这一层：**C-ABI 导出 + Swift 封装**，使 iOS 的能力面与 Node 对齐。

### 为什么值得做

`force-physical`（绕开 VPN、用用户真实 IP 出网）是这套栈的核心卖点——业务侧拿它请求接入点接口，避免在 VPN 环境下被调度到错误地域的节点。Node 侧已验证有效（同一台机器上 `os` 策略出 VPN 出口，`force-physical` 出物理网卡真实 IP）。iOS 是 rtc-client 的目标平台之一，这个能力目前在 iOS 上完全用不了。

## 目标与非目标

### 目标

1. `ios_bridge.h` / `.mm` 导出 HTTP 与 WS 两套 C-ABI，风格与既有 `tt_connector_*` 一致。
2. `src/swift/` 增加对应的 Swift 封装，HTTP 用 `async/await`，WS 用 delegate。
3. QUICTest 加验证入口，在模拟器上实跑，拿到真实运行结果作为证据。

### 非目标（YAGNI）

- **不暴露 `sendPacket`**。`WSConnection::SendPacket(SMPacketPtr)` 在 C++ 层存在（SMP 帧走 WS 传输），但 Node 侧也没有暴露它，没有已知的 iOS 消费方需要。等真有需求再加。
- **不做 WS 服务端**。`WSServer` 不在此范围。
- **不改动任何现有 C-ABI 或 Swift 类型**。本设计纯增量，`TTSignalConnector` / `TTSignalConnection` / `TTSignalHandler` 一行不动，livekit-client-swift 的现有集成不受影响。
- **不把 Swift 源码打进 xcframework**。维持现状：xcframework 只分发 C 头 + modulemap，`src/swift/*.swift` 由消费方直接加入自己的 target。

## 设计决策

四个决策点及其取舍：

| 决策 | 选定 | 理由 |
|---|---|---|
| Swift API 形态 | HTTP 用 `async/await`，WS 用 delegate | HTTP 的请求-响应天然是 `await` 一行；WS 的事件流沿用现有 `TTSignalHandler` 的 delegate 风格，与 livekit-client-swift 的 `SignalingTransport` 模式一致 |
| 配置类型 | 独立 `TTSignalHttpConfig` / `TTSignalWSConfig`，公共部分内嵌 `TTSignalNetConfig` | 与 Node 形状一致。复用 `TTSignalConfig`（200+ 行）会在公共 API 上摆一堆对 HTTP/WS 无效的字段（alpn / congestionControl / proxyUrl / maxStreams），调用方无从判断哪些真的生效 |
| HTTP 响应传递 | 扁平 struct + headers 平行数组 | 一次 completion 拿全，天然映射到 Swift 单次 continuation resume。headers 用 `names[]/values[]/count` 而非拼接字符串，避开 header value 含冒号/换行时的解析歧义 |
| 验证深度 | QUICTest 加入口，模拟器实跑 | 编译+符号验证证明不了回调桥接、continuation 恢复、线程模型这些真正容易错的部分 |

## C-ABI 设计

新增内容全部追加在 `ios_bridge.h` 现有声明之后，`extern "C"` 块内。

### 公共约定

沿用现有 `on_recv_data` 的约定：**回调参数里的指针只在回调期间有效**，Swift 侧必须立刻拷贝。所有 `int32_t` 错误码是 `BCRESULT`，与 Node 侧同一套跨语言契约值（`64 BC_R_NO_PHYSICAL_INTERFACE`、`69 BC_R_PIN_FAILED`、`70 BC_R_ROUTE_MISMATCH`、`71 BC_R_DNS_FAILED`、`72 BC_R_TLS_VERIFY_FAILED`、`73 BC_R_RESPONSE_TOO_LARGE`、`74 BC_R_WS_HANDSHAKE_FAILED`）。

### HTTP

```c
typedef struct TTHttpConnector* TTHttpConnectorRef;

typedef struct {
    int32_t     vpnPolicy;          // -1 = 平台默认；其余同 TTVpnPolicy
    const char* caCerts;            // 额外信任的 PEM，NULL/"" = 不加
    const char* spkiPin;
    int32_t     insecureSkipVerify; // 非 0 = 跳过证书校验（别在生产用）
    const char* dnsServers;
    uint32_t    dnsTimeoutMs;
    uint32_t    drainTimeoutMs;
    int32_t     logLevel;           // 1 DEBUG ... 5 FATAL，0 = 默认
    size_t      maxResponseBytes;   // 0 = 用默认 8MiB
} TTHttpConfig;

typedef struct {
    const char* method;             // NULL => "GET"
    const char* url;                // 必填，http:// 或 https://
    const char* const* headerNames;
    const char* const* headerValues;
    size_t      headerCount;
    const uint8_t* body;
    size_t      bodyLen;
    uint32_t    timeoutMs;          // 0 = 用默认
    const char* resolvedIp;         // 非 NULL 时跳过 DNS，直接连这个 IP
} TTHttpRequest;

typedef struct {
    int32_t     status;
    const char* reason;
    const char* const* headerNames;
    const char* const* headerValues;
    size_t      headerCount;
    const uint8_t* body;
    size_t      bodyLen;
    const char* peerIp;
    uint32_t    boundIfIndex;       // 非 0 = 真的绑上了物理网卡
    const char* pinMethod;          // "IP_BOUND_IF" / "SO_BINDTODEVICE" / ""
} TTHttpResponse;

// error == 0 时 resp 非 NULL；error != 0 时 resp 为 NULL，看 errName/errMessage。
// 4xx/5xx 算正常响应（error == 0），与 Node 侧一致。
typedef void (*TTHttpCompletion)(void* userdata,
                                 int32_t error,
                                 const char* errName,
                                 const char* errMessage,
                                 const TTHttpResponse* resp);

typedef void (*TTCloseDone)(void* userdata);   // HTTP 与 WS 连接器共用

TTHttpConnectorRef tt_http_connector_create(const TTHttpConfig* config);
// 返回值仅表示"是否成功受理"。非 0 时 completion 不会被调用。
int32_t tt_http_request(TTHttpConnectorRef connector,
                        const TTHttpRequest* req,
                        TTHttpCompletion completion,
                        void* userdata);
void tt_http_connector_close(TTHttpConnectorRef connector,
                             TTCloseDone done, void* userdata);
void tt_http_connector_destroy(TTHttpConnectorRef connector);
```

### WS

```c
typedef struct TTWSConnector*  TTWSConnectorRef;
typedef struct TTWSConnection* TTWSConnectionRef;

typedef struct {
    // --- 与 TTHttpConfig 前 8 项同义 ---
    int32_t     vpnPolicy;
    const char* caCerts;
    const char* spkiPin;
    int32_t     insecureSkipVerify;
    const char* dnsServers;
    uint32_t    dnsTimeoutMs;
    uint32_t    drainTimeoutMs;
    int32_t     logLevel;
    // --- WS 专有 ---
    uint32_t    connectTimeoutMs;
    uint32_t    pingIntervalMs;
    uint32_t    idleTimeoutMs;
    size_t      maxFrameBytes;
    const char* clientCertFile;
    const char* clientKeyFile;
    const char* clientKeyPassword;
} TTWSConfig;

typedef struct {
    void (*on_connect_result)(void* ud, int32_t error, const char* message);
    void (*on_text)     (void* ud, const char* text, size_t len);
    void (*on_data)     (void* ud, const uint8_t* data, size_t len);
    void (*on_closed)   (void* ud, const char* reason);
    void (*on_exception)(void* ud, const char* errMsg);
    // 见「生命周期」一节：由 bridge 在确定不再有任何回调时调用一次，
    // Swift 侧在这里释放 box。**每个 connection 恰好一次**。
    void (*on_release)  (void* ud);
} TTWSHandlerVTable;

TTWSConnectorRef  tt_ws_connector_create(const TTWSConfig* config);
TTWSConnectionRef tt_ws_connector_create_connection(TTWSConnectorRef connector,
                        const TTWSHandlerVTable* vtable, void* userdata);
void    tt_ws_connector_close(TTWSConnectorRef, TTCloseDone done, void* ud);
void    tt_ws_connector_destroy(TTWSConnectorRef);

int32_t tt_ws_connection_connect(TTWSConnectionRef, const char* url, uint32_t timeoutMs);
// 必须在 connect 之前调用，之后调用返回 BC_R_ALREADYRUNNING。
int32_t tt_ws_connection_set_request_header(TTWSConnectionRef,
                                            const char* name, const char* value);
int32_t tt_ws_connection_send_text(TTWSConnectionRef, const char* text, size_t len);
int32_t tt_ws_connection_send_data(TTWSConnectionRef, const uint8_t* data, size_t len);
int32_t tt_ws_connection_send_ping(TTWSConnectionRef);
uint32_t tt_ws_connection_bound_if(TTWSConnectionRef);
void    tt_ws_connection_close(TTWSConnectionRef);
void    tt_ws_connection_destroy(TTWSConnectionRef);
```

收发与生命周期方法集与 Node 的 `WsConnector` / `WsConnection` 对齐（`createConnection` / `close`；`connect` / `sendText` / `sendData` / `sendPing` / `close`）。

**不暴露 Node 侧的 `info()` / `stats()`**：那两个是调试用的状态快照，iOS 侧没有已知消费方。唯一必须留下的是 `tt_ws_connection_bound_if`——验证 `force-physical` 是否真的绑上物理网卡要靠它，HTTP 侧对应的字段则已经在 `TTHttpResponse.boundIfIndex` 里。日后若需要完整快照再补 `tt_ws_connection_info`。

## 生命周期与线程模型

这是本设计最容易写错的部分，单列一节。

### 回调线程

`IWSConnectionHandler` 的回调发生在该连接对应 `TcpChannel` 的事件循环线程；`HttpConnector` 的 completion 发生在工作线程。**都不是主线程**，与现有 `TTSignalHandler` 的约定一致（"callbacks are dispatched on internal worker threads — implementations must hop to MainActor themselves"）。Swift 文档需重申这一点。

在 worker 线程 resume continuation 是合法的，但 `await` 之后的代码会继续在该线程上跑一段，**不要在那里做耗时操作**——Node 侧有同样的坑，`src/js/index.js` 的注释专门写了一段（"resolve 之后不要在 then 里做耗时的事"）。

### HTTP 的 box 生命周期

`userdata` 是 `Unmanaged.passRetained(Box).toOpaque()`。C++ 契约保证 completion **恰好一次**，所以 completion 里直接 `takeRetainedValue()` 取回并释放，随即 resume continuation。

`tt_http_request` 返回非 0 表示没受理（参数错误等），此时 completion 不会来，调用方必须自己 `takeRetainedValue()` 释放，否则泄漏。Swift 封装负责处理这个分支，不让调用方看见。

### WS 的 box 生命周期 ← 最易出错

`WSConnector.h` 的生命周期契约（该文件顶部第 1-3 条）是：

1. `Connect()` 返回非 `BC_R_SUCCESS` 时**不会有任何回调**。
2. `Connect()` 返回 `BC_R_SUCCESS` 之后：
   - `OnConnectResult` 恰好回调一次；
   - `result == BC_R_SUCCESS` 时，之后还会恰好回调一次 `OnClosed`；
   - `result != BC_R_SUCCESS` 时**不再有 `OnClosed`**（握手没成功，也就没有"连接关闭"这件事）。
3. handler 必须活到最后一次回调返回。

直接把这套契约暴露给 Swift 会逼调用方去推理"这次该不该释放"，写错就是泄漏或 use-after-free。**因此 vtable 增加 `on_release`**：由 bridge 侧的 adapter 依据上述契约判断"不再有任何回调"，然后调用它恰好一次；Swift 侧只需在 `on_release` 里 `takeRetainedValue()`，不必理解契约细节。

adapter 的释放判定：

| 情形 | `on_release` 触发点 |
|---|---|
| `Connect()` 同步返回非 SUCCESS | `tt_ws_connection_connect` 返回前 |
| 从未调用 `connect` 就 `destroy` | `tt_ws_connection_destroy` 内 |
| `OnConnectResult(error != 0)` | 该回调返回后 |
| `OnConnectResult(error == 0)` → `OnClosed` | `OnClosed` 返回后 |
| 连接器先于连接关闭 | 连接器放弃路径上补发（`~WSConnector` 的放弃路径下 `OnClosed` 永远不会来，Node 侧对此有相同处理） |

adapter 内部用一个原子标志保证 `on_release` 只发一次。

### 句柄所有权

`WSConnPtr` 是 `shared_ptr`，连接器会一直持有直到收尾，**业务放手不等于对象销毁**。`TTWSConnectionRef` 是 bridge 分配的包装对象（持有 `WSConnPtr` + adapter），`tt_ws_connection_destroy` 只释放这层包装。Swift 的 `TTSignalWSConnection` 在 `deinit` 里调 `destroy`。

## Swift API

四个新文件，放在 `src/swift/`：

| 文件 | 类型 |
|---|---|
| `TTSignalNetConfig.swift` | `TTSignalNetConfig`（公共配置 + `toC()`） |
| `TTSignalError.swift` | `TTSignalError: Error`，字段 `result` / `errName` / `errMessage` |
| `TTSignalHttp.swift` | `TTSignalHttpConfig` / `TTSignalHttpResponse` / `TTSignalHttpConnector` |
| `TTSignalWS.swift` | `TTSignalWSConfig` / `TTSignalWSHandler` / `TTSignalWSConnector` / `TTSignalWSConnection` |

### 用法

```swift
// HTTP —— 一次性请求，复用单例，用完 close()
var cfg = TTSignalHttpConfig()
cfg.net.vpnPolicy = .forcePhysical
let http = TTSignalHttpConnector(config: cfg)
defer { Task { await http.close() } }

let resp = try await http.request(url: "https://ipinfo.io/json")
print(resp.status, resp.bodyText ?? "", resp.boundIfIndex, resp.pinMethod)

// WS —— 长连接，delegate
final class MyHandler: TTSignalWSHandler {
    func onConnectResult(_ c: TTSignalWSConnection, error: Int32, message: String?) {}
    func onText(_ c: TTSignalWSConnection, text: String) {}
    func onData(_ c: TTSignalWSConnection, data: Data) {}
    func onClosed(_ c: TTSignalWSConnection, reason: String?) {}
    func onException(_ c: TTSignalWSConnection, errMsg: String) {}
}

var wcfg = TTSignalWSConfig()
wcfg.net.vpnPolicy = .forcePhysical
let ws   = TTSignalWSConnector(config: wcfg)
let conn = ws.createConnection(handler: handler)
try conn.connect(url: "wss://host/path", timeoutMs: 5000)
try conn.sendText("hello")
```

`TTSignalHttpResponse` 提供 `headers: [String: String]`（从平行数组组装，header 名大小写不敏感地保留原样）、`body: Data`、便利属性 `bodyText: String?`。

`onException` 与 `onData`/`onText` 之外的方法通过 protocol extension 给默认空实现，与现有 `TTSignalHandler` 的做法一致（`onStreamDataAcked` / `onStreamDataSent` 就是这么处理的）。

### 错误映射

`request` 抛 `TTSignalError`，三个字段原样透传原生层的值，不重新包装——与 Node 侧同一约定。`errMessage` 在 `force-physical` 失败时写得很细（含对端 IP、peerClass、内核把包判给了哪块网卡、该怎么改配置），**排查 force-physical 只能看这一段**，Swift 封装不得吞掉或改写它。

发送类接口（`sendText` / `sendData` / `sendPing`）抛出的错误只有 `result`，没有 `errName` / `errMessage`——原生发送接口只给错误码。这一点需在文档注释里写明，与 Node 侧文档保持一致。

## 验证方案

QUICTest 增加一个验证页面，模拟器实跑：

1. **HTTP + vpnPolicy 对照**：对 `https://ipinfo.io/json` 各发一次 `os` 与 `forcePhysical` 请求，展示返回的 IP、`boundIfIndex`、`pinMethod`。预期与第一轮 Node 侧结果同构：`os` 走系统默认路由，`forcePhysical` 拿到物理网卡的真实 IP 且 `boundIfIndex != 0`、`pinMethod == "IP_BOUND_IF"`。
2. **HTTP 错误路径**：请求一个必然失败的地址，确认 `TTSignalError` 的三个字段都非空。
3. **WS 收发**：连一个公开 echo 服务，`sendText` 后确认 `onText` 收到相同内容，再 `close()` 确认 `onClosed` 到达。
4. **无泄漏**：连接失败与正常关闭两条路径各跑一次，确认 `on_release` 各触发一次（临时加日志断言，验证后移除）。

补充的静态验证：`libTTSignal.a` 中 `_tt_http_*` / `_tt_ws_*` 出现在导出表（大写 `T`），且除 `_tt_*` 外无新增外部可见符号——即符号防火墙未被破坏。

## 构建集成

- `ios_bridge.mm` 需新增 `#include "HttpConnector.h"` 与 `#include "WSConnector.h"`。这两个文件已随 llhttp 一同解禁，可直接使用。
- `src/CMakeLists.txt` **无需改动**：`TT_IOS_BRIDGE_SOURCES` 仍是 `ios_bridge.mm`，`TT_IOS_CORE_SRC` 的 glob 已经涵盖 `HttpConnector.cpp` 等四个文件。
- 新增的 `_tt_http_*` / `_tt_ws_*` 自动落入 `build-xcframework.sh` 的 `-exported_symbols_list` 白名单（模式是 `_tt_*`），**符号防火墙配置无需修改**。
- Swift 文件放 `src/swift/`，不进 xcframework，与现有七个文件同样由消费方加入 target。

## 风险

1. **WS 连接失败路径的 box 释放**。这是泄漏与 use-after-free 的高发点，`on_release` 的设计就是为了把判定收敛到 bridge 一侧。实现时逐条对齐 `WSConnector.h` 的三条契约，并在验证方案第 4 项显式覆盖。
2. **`async/await` 在 iOS 13**。部署目标是 13.0，Swift concurrency 需要 Xcode 13.2+ 的 back-deployment。若 livekit-client-swift 使用更老的 Xcode，`TTSignalHttp.swift` 会编不过。缓解：HTTP 与 WS 分在不同文件，WS 那半边（delegate 风格）不依赖 concurrency，可单独使用。
3. **在 worker 线程 resume continuation**。合法但需文档写明约束，见「回调线程」一节。
