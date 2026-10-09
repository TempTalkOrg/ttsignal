# TCP/TLS/HTTP/WS 客户端栈：支持 https/ws/wss 与真实 IP 出网

日期：2026-08-17
状态：设计已确认，待实现

## 背景与问题

`src/cpp/WS*`、`SSLayer.*`、`jni/JNI_WS*` 这 12 个文件目前是 `~/work/jmp/jmp/src/cpp/` 对应文件的**逐字节拷贝**（`diff` 全部为空），未纳入 `src/CMakeLists.txt`，处于「拷进来了但一行没改」的状态。

同时，业务侧有一个新需求：在开启 VPN / 虚拟网卡的环境下，用**用户真实 IP** 发起 HTTPS 请求访问 `getServiceCallUrl` 接口，以获取准确的接入点。走 VPN 出口 IP 拿到的接入点是错的（会被调度到 VPN 出口所在地域的节点）。

这两件事共用同一套底座：TCP + TLS + 网卡绑定 + DNS。

### 现状缺口

**一、拷贝过来的代码编不过**

| 缺口 | 位置 |
|---|---|
| `Acceptor.h` 不存在 | `WSConnector.h:18`、`WSServer.h` include 它 |
| `JMPacket.h` 不存在 | `WSConnector.h:22`、`WSParser.h:12` include 它 |
| `IWSConnectorHandler` / `IWSConnectionHandler` / `IWSServerHandler` 未定义 | jmp 定义在它自己的 `Interface.h:185-320`，ttsignal 的 `Interface.h` 只有 SMP 系接口 |
| 命名空间不匹配 | 全文 `using namespace JMP` / `JMPacketPtr`，本项目是 `SMP` / `SMPacketPtr` |
| JNI 类路径写死 | `JNI_WSConnectorWrap.cpp:59,144,267...` 全是 `com/jd/jmp/*`，本项目是 `org/difft/android/smp/*` |
| 未纳入构建 | `src/CMakeLists.txt` 无任何 WS/SSLayer 条目 |

好消息：`deps/env`（BC 框架 + HTTP 库）和 `deps/llhttp` 都在，且 `deps/env/src` 已经通过 `add_subdirectory` 纳入构建（`CMakeLists.txt:90`），底座是齐的。

**二、`WSConnection` 不支持域名**

`WSConnector.cpp:502-534`：`HTTPProtocol::ParseAddrFromUrl` 取出 host 后，直接交给 `bc_net_pton()`。`bc_net_pton` 只认 IP 字面量，传域名必然返回 `<= 0`，走到 `result = BC_R_HOSTUNREACH`。**当前实现只能连 IP，没有任何 DNS 解析。**

**三、wss 的 TLS 是半成品**

`WSConnector.cpp:546-592` 建了 `SSL_CTX`，但：

- 没有 `SSL_set_tlsext_host_name()` — 缺 SNI，多租户 TLS 服务端会返回错误证书或直接拒绝
- 没有 `SSL_CTX_set_verify()` / CA 加载 / `X509_check_host()` — **服务端证书完全不校验**
- `SSL_CTX_set_min_proto_version(ssl_ctx_, TLS1_1_VERSION)` — TLS 1.1 已被弃用
- `WSConnector.cpp:562` `if (!config_.private_key_password)` 判空写反了：只在密码**为空**时才去设密码回调
- `WSConnector.cpp:558-561` 条件里 `config_.certificate_file != nullptr` 出现两次，第二次本意应是 `private_key_file != nullptr`；结果 `strlen(config_.private_key_file)` 在 `private_key_file` 为 NULL 时会解引用空指针

**四、完全没有 HTTP 请求能力**

`WSConnection` 只会发 WebSocket Upgrade 握手（`_UpgradeProtocol` / `_Inter_Send_Request`），没有「发普通 GET/POST → 收完整 response body」的路径。`LLHTTPParser` 的 `http_on_status` / `http_on_headers_complete` / `http_on_message_complete` 回调是现成的，但没有一条码把 body 收集起来交给调用方。`getServiceCallUrl` 无从实现。

`deps/env/src/HTTP/HTTPConnOut.cpp:445-453` 那个 HTTP 客户端解析出了 `bSSL` 却从不使用它——**不支持 TLS**，且它是 deps 里的第三方代码，不在本次改造范围内。

**五、TCP 路径完全没有网卡绑定**

`WSConnector.cpp:617` 直接 `socket_->Connect()`，没有任何 `IP_BOUND_IF` / `SO_BINDTODEVICE` / `IP_UNICAST_IF`。项目里现成的 `VpnPolicy` / `NetworkRouteLookup` / `INetworkPathMonitor` 三套设施，QUIC 侧（`UDPSender`）用得很完整，TCP 侧一个都没接上。

**六、绑了 TCP 也不够——DNS 会被劫持**

这是本次改造真正的难点。在 Clash / Surge / mihomo 的 TUN 全局模式下：

1. `getaddrinfo` 走系统 DNS，请求被 VPN 劫持
2. fake-IP 模式返回 `198.18.0.0/15` 段的假地址
3. 此时即使把 TCP socket 硬绑到物理网卡，目标 IP 本身是假的，物理网卡上根本没有到 `198.18.x.x` 的路由，连接直接黑洞

所以「用真实 IP 出网」必须**从 DNS 开始就绕过 VPN**，只绑 TCP 是不够的。

## 目标与非目标

**目标**

1. 让 `src/cpp/WS*` 真正编译进 ttsignal，支持 `ws://` / `wss://`，且支持域名
2. 新增通用 HTTP 客户端能力，支持 `http://` / `https://`，可发任意 method + headers + body，收完整 response
3. 三档 `vpnPolicy` 在 TCP 路径和 DNS 路径上都生效，`force-physical` 下**要么用真实 IP 出网，要么明确失败**
4. 把 `UDPSender::_InitSocket` 里的平台绑定代码抽成可复用模块，QUIC 侧与 TCP 侧共用一份实现

**非目标**

- **不做服务端。** `WSServer.h/.cpp`（2118 行）和 `Acceptor` 本轮不移植
- **不接绑定层。** 本轮只做 C++ 核心层 + 命令行验证工具，JNI / NAPI / iOS Swift 下一轮
- **不硬编码任何业务接口。** `getServiceCallUrl` 只是上层业务的一次普通 GET，ttsignal 不认识这个接口名、不拼它的 URL、不解析它的响应体
- **不做自动降级重连。** 与 `vpnPolicy` 既有约定一致，策略降级是业务的决定
- 不做 HTTP 代理、不做 HTTP/2、不做连接池复用（一次请求一条连接）

## 设计

### 一、模块划分

```
                    ┌─────────────────┐  ┌──────────────────┐
                    │  WSConnection   │  │  HttpConnection  │
                    │  (WS 帧协议)     │  │  (HTTP 请求响应)  │
                    └────────┬────────┘  └────────┬─────────┘
                             └──────────┬─────────┘
                                        ▼
                              ┌───────────────────┐
                              │    TcpChannel     │
                              │  连接状态机 + 收发  │
                              └─────────┬─────────┘
                    ┌───────────────────┼───────────────────┐
                    ▼                   ▼                   ▼
            ┌──────────────┐   ┌────────────────┐   ┌─────────────┐
            │ DnsResolver  │   │  SocketPinner  │   │ TlsContext  │
            │  域名 → IP    │   │  fd → 网卡      │   │ SSL_CTX 工厂 │
            └───────┬──────┘   └────────────────┘   └─────────────┘
                    └──────────────────┬┘
                                       ▼
                        (已有) VpnPolicy / NetworkRouteLookup
                               / INetworkPathMonitor
```

`UDPSender::_InitSocket` 同时改为调用 `SocketPinner`，QUIC 侧与 TCP 侧共享同一份平台代码。

### 二、文件清单

**新增**

| 文件 | 职责 |
|---|---|
| `src/cpp/TTErrors.h` | 集中定义 `BC_R_NRESULTS + N` 系列错误码 |
| `src/cpp/SocketPinner.h/.cpp` | 把 socket fd 绑到指定网卡。纯 C 接口，无状态 |
| `src/cpp/DnsResolver.h/.cpp` | UDP DNS 客户端（A/AAAA），socket 过 SocketPinner |
| `src/cpp/TlsContext.h/.cpp` | `SSL_CTX` 工厂：SNI、CA 加载、SPKI pin、hostname 校验 |
| `src/cpp/TcpChannel.h/.cpp` | DNS→socket→pin→connect→TLS→读写 的统一底座 |
| `src/cpp/HttpConnector.h/.cpp` | `HttpConnector` / `HttpConnection` / `HttpRequest` / `HttpResponse` |
| `src/cpp/tests/SocketPinner_test.cpp` | 参数校验与平台分支的可测部分 |
| `src/cpp/tests/DnsResolver_test.cpp` | DNS 报文编解码（含 name compression） |
| `tools/httpget.cpp` | 端到端验证用命令行工具 |

**改造**

| 文件 | 改动 |
|---|---|
| `src/cpp/WSConnector.h/.cpp` | 连接路径换成 `TcpChannel`；`JMP`→`SMP`；支持域名；删掉 `Acceptor.h` include |
| `src/cpp/WSParser.h/.cpp` | `JMPacket`→`SMPacket`，去掉 `using namespace JMP` |
| `src/cpp/SSLayer.h/.cpp` | 接入 `TlsContext`，修复 SNI 缺失 |
| `src/cpp/Interface.h` | 新增 `IWSConnectorHandler` / `IWSConnectionHandler` / `IHttpConnectorHandler` / `IHttpRequestHandler` |
| `src/cpp/UDPSender.cpp` | `_InitSocket` 的绑定代码改调 `SocketPinner`。**行为不变，纯搬家** |
| `src/CMakeLists.txt` | 纳入新增文件（三处 source list：Node addon / JNI / iOS） |

**删除**

| 文件 | 原因 |
|---|---|
| `src/cpp/WSServer.h/.cpp` | 本轮不做服务端。是 jmp 的逐字节拷贝，下轮需要时重新拷 |
| `src/cpp/jni/JNI_WSConnectorWrap.h/.cpp` | 本轮不接绑定层，且引用 `com/jd/jmp/*` 类路径 |
| `src/cpp/jni/JNI_WSServerWrap.h/.cpp` | 同上 |

留着编不过的死代码比删掉更有害：它引用 `Acceptor.h`、`JMPacket.h`、`com/jd/jmp/*`，会让后来人误以为这些依赖存在。

### 三、SocketPinner

唯一一处知道 `IP_BOUND_IF` / `IPV6_BOUND_IF` / `SO_BINDTODEVICE` / `IP_UNICAST_IF` / `IPV6_UNICAST_IF` / `android_setsocknetwork` 的地方。

```c
typedef enum {
    TT_PIN_OK          = 0,  // 绑定成功
    TT_PIN_NOT_NEEDED  = 1,  // ifIndex == 0，调用方未要求绑定
    TT_PIN_UNSUPPORTED = 2,  // 平台无此能力（如 iOS 的部分场景）
    TT_PIN_FAILED      = 3,  // setsockopt 失败，errno 见 outErrno
} TTPinResult;

typedef struct TTPinRequest {
    int         fd;
    uint32_t    ifIndex;           // 0 = 不绑定
    uint64_t    androidNetHandle;  // Android 用 net handle，非 ifIndex
    int         ipv6;              // 非 0 时同时设置 IPv6 选项
    int         isTcp;             // 非 0 表示 TCP；见下方约束
    TTVpnPolicy policy;            // FORCE_PHYSICAL 时优先 SO_BINDTODEVICE
    void*       loggerCtx;         // LogQ 上下文，可为 NULL
} TTPinRequest;

// outMethod 写入实际生效的绑定手段名（"IP_BOUND_IF" / "SO_BINDTODEVICE" /
// "IP_UNICAST_IF" / "android_setsocknetwork"），供日志与诊断使用。可传 NULL。
TTPinResult tt_socket_pin(const TTPinRequest* req,
                          char* outMethod, size_t outMethodLen,
                          int* outErrno);

// 撤销绑定。UDPSender 的 fake-IP unpin 路径使用。
// force-physical 下调用方不应调用本函数（既有约定，见 652a603）。
TTPinResult tt_socket_unpin(int fd, int ipv6, void* loggerCtx);
```

**TCP 的硬约束**：`tt_socket_pin` 必须在 `connect()` **之前**调用。

- macOS/iOS：`IP_BOUND_IF` 在已连接 socket 上设置对已缓存的路由无效
- Linux：`IP_UNICAST_IF` 影响的是路由查找，TCP 的路由在 `connect()` 时确定；`SO_BINDTODEVICE` 在 connect 后设置不改变已建立连接
- Windows：`IP_UNICAST_IF` 文档明确要求在 connect 前设置

这条约束写进头文件注释，并由 `TcpChannel` 的状态机保证（`CONNECTING` 状态进入前完成 pin）。

**从 `UDPSender::_InitSocket` 抽取时保持行为不变**：Android 走 `android_setsocknetwork`（`UDPSender.cpp:530-549`）、Apple 走 `IP_BOUND_IF`（`550-577`）、Linux 在 `force-physical` 下优先 `SO_BINDTODEVICE` 失败回落 `IP_UNICAST_IF`（`578-635`）、Windows 走需要 `htonl` 的 `IP_UNICAST_IF`。这些分支连同注释一起搬过去，`UDPSender.cpp` 只保留调用点。

TCP 与 UDP 的唯一行为差异是 Linux 上的回落告警措辞：`IP_UNICAST_IF` 对已 connect 的 UDP socket 在旧内核上被静默忽略（`UDPSender.cpp:588-600` 记录的坑），但对 connect 前的 TCP socket 是有效的。所以 TCP 路径下 `SO_BINDTODEVICE` 失败时的告警级别降为 `_INFO_`，不再声称「可能拿不到真实 IP」。

### 四、DnsResolver

```cpp
struct DnsConfig {
    TTVpnPolicy              policy;
    uint32_t                 ifIndex;        // 绑定用；policy == OS 时忽略
    std::vector<std::string> servers;        // 默认 {"223.5.5.5", "8.8.8.8"}
    uint32_t                 timeoutMs;      // 单台 server，默认 2000
    bool                     preferIpv6;     // 默认 false
    void*                    loggerCtx;
};

struct DnsResult {
    BCRESULT                 result;
    std::vector<BCSockAddrS> addrs;          // 按 preferIpv6 排序
    std::string              errMessage;     // 失败时说明卡在哪一步
};

class DnsResolver {
public:
    // 同步调用，必须在 BCTaskMgr 线程池里执行，不能在事件循环线程调用
    static DnsResult Resolve(const std::string& host, const DnsConfig& cfg);
    static void      ClearCache();
};
```

**策略分支**

- `policy == OS`：直接 `getaddrinfo`。不走自建查询，行为与今天的系统解析一致
- `policy == PREFER_PHYSICAL`：先试自建查询；socket 绑定失败或所有 DNS server 都超时，回落 `getaddrinfo` 并打 `_WARN_`
- `policy == FORCE_PHYSICAL`：只走自建查询。socket 绑定失败或全部 server 超时即返回失败，**不回落**

**自建查询实现**

自己拼 DNS 报文，不引入新依赖。查询侧只需要构造 header（12 字节，`RD=1`）+ QNAME（label 长度前缀编码）+ QTYPE/QCLASS。响应侧需要完整解析 answer section，**必须处理 name compression 指针**（`0xC0` 前缀的 2 字节偏移）——这是 DNS 解析最容易写错的地方，且必须防御指针成环（限制跳转次数，超过 16 次判为畸形报文）和越界偏移。

传输用普通 UDP socket（不经 `BCSocket`，因为需要同步 `poll` 超时语义），`socket()` 之后立刻 `tt_socket_pin()`，再 `sendto` 到 `server:53`。同时发 A 和 AAAA 两个查询（不同 txid），收齐或超时后合并结果。

对每台 server 串行重试，全部失败才返回错误。`errMessage` 记录每台 server 的失败原因。

**缓存**

进程内 `host → (addrs, expireAt)` 小缓存，TTL 取 answer 中所有记录 TTL 的最小值，并夹在 `[10s, 600s]`。缓存 key 必须包含 `policy` 和 `ifIndex`——同一域名在物理网卡和 VPN 下解析结果不同，混用会导致极难排查的串味问题。切网时（`INetworkPathMonitor` 回调）清空缓存。

**截断处理**：响应设置 `TC` 位时，本轮不实现 TCP fallback，直接按「结果不完整」处理——取已拿到的记录，若为空则视为该 server 失败。A/AAAA 查询几乎不会超过 512 字节，代价可接受。

### 五、TlsContext

```cpp
struct TlsConfig {
    std::string caCertsPem;         // 空 → 回落系统信任库
    std::string spkiPin;            // base64 SPKI pin，可选
    std::string clientCertFile;     // 双向 TLS，可选
    std::string clientKeyFile;
    std::string clientKeyPassword;
    bool        insecureSkipVerify = false;  // 仅自签调试
    void*       loggerCtx = nullptr;
};

class TlsContext {
public:
    // 返回的 SSL_CTX 由调用方 SSL_CTX_free
    static SSL_CTX* Create(const TlsConfig& cfg, std::string& outErr);
    // 每条连接调用：设 SNI + hostname 校验期望值
    static BCRESULT PrepareSsl(SSL* ssl, const std::string& host,
                               const TlsConfig& cfg, std::string& outErr);
};
```

**校验逻辑复用 `SMPConnector.cpp:3441-3536` 已经验证过的实现**（CA store 构建 → `X509_verify_cert` → `X509_check_host` → 失败时打印完整证书链和本地 CA 列表用于诊断），改写成不依赖 xquic 回调签名的独立函数。`computeSpkiPinBase64` / `canonicalizeSpkiPin` / `addTrustedCAsToStore` 这三个 helper 从 `SMPConnector.cpp` 提到 `TlsContext.cpp`，`SMPConnector` 改为引用——避免两份实现漂移。

**与 SMP 的唯一行为差异**：`caCertsPem` 为空时，SMP 是「跳过校验」，TLS 侧是「回落系统信任库」。理由是 https/wss 常用于访问公网 API（如 `getServiceCallUrl`），默认不校验存在中间人风险；而 SMP 连的是自家 SFU，行为保持不变。

系统信任库加载顺序：

1. `SSL_CTX_set_default_verify_paths()`
2. Android 额外加 `/system/etc/security/cacerts`（目录形式，`SSL_CTX_load_verify_locations(ctx, NULL, path)`）
3. 以上都拿不到任何 CA 时，若 `insecureSkipVerify == false` 则 `Create` 失败并在 `outErr` 说明——**静默降级为不校验是不可接受的**

其余固定项：`SSL_CTX_set_min_proto_version(TLS1_2_VERSION)`（从 TLS 1.1 提上来）、`SSL_VERIFY_PEER`、`SSL_set_tlsext_host_name(ssl, host)`。SNI 只在 host 是域名时设置，IP 字面量不设（RFC 6066 禁止）。

顺带修掉背景里列出的两个既有 bug：`private_key_password` 的反向判空，以及 `private_key_file` 的空指针解引用。

### 六、TcpChannel

```cpp
class ITcpChannelHandler {
public:
    virtual void OnChannelReady()                              = 0;
    virtual void OnChannelData(const void* data, size_t size)  = 0;
    virtual void OnChannelClosed(BCRESULT result,
                                 const std::string& reason)    = 0;
};

struct TcpChannelConfig {
    std::string  host;            // 域名或 IP 字面量
    uint16_t     port;
    bool         tls;
    std::string  resolvedIp;      // 非空则跳过 DNS
    TTVpnPolicy  policy;
    DnsConfig    dns;
    TlsConfig    tls_cfg;
    uint32_t     connectTimeoutMs;
    void*        loggerCtx;
};

class TcpChannel {
public:
    BCRESULT Open(const TcpChannelConfig& cfg, ITcpChannelHandler* h);
    BCRESULT Send(BufferPtr buf);
    void     Close();
    std::string PeerIp() const;
    uint32_t    BoundIfIndex() const;   // 实际绑定的网卡，0 表示未绑
    std::string PinMethod() const;      // 实际生效的绑定手段，供诊断
};
```

状态机：

```
IDLE → RESOLVING → PINNING → CONNECTING → TLS_HANDSHAKE → READY → CLOSING → CLOSED
                                              ↑ tls == false 时跳过
```

`RESOLVING` 在 `BCTaskMgr` 线程池执行（`DnsResolver::Resolve` 是同步的），完成后 post 回事件循环继续。其余状态全部在事件循环线程，复用 `WSConnection` 现有的 `BCEventQueue` + `BCSocket` 异步回调模式（`_ConnectDoneCallback` / `_RecvDoneCallback` / `_SendDoneCallback`）。

TLS 收发夹在中间：`OnChannelData` 之前先过 `SSLayer::ReadFromSSL`，`Send` 之前先过 `SSLayer::WriteToSSL`，与 `WSConnection` 今天的做法一致（`WSConnector.cpp:669-703, 815-817`）。

### 七、VPN 绕过完整数据流

以 `force-physical` 下一次 HTTPS 请求为例：

```
request({url:'https://api.example.com/getServiceCallUrl',
         vpnPolicy:'force-physical'})
  │
  ├─1 选网卡：INetworkPathMonitor 当前 active ifIndex
  │    └─ 取不到，或选出的是虚拟网卡（utun/ipsec/ppp/tun/wintun）
  │       → 失败 BC_R_NO_PHYSICAL_INTERFACE，不做任何回落
  │
  ├─2 DNS：DnsResolver::Resolve("api.example.com", {force-physical, ifIndex})
  │    ├─ UDP socket → tt_socket_pin(fd, ifIndex, UDP) → 失败即返回失败
  │    ├─ sendto(223.5.5.5:53) 查 A + AAAA，poll 超时 2s，失败换下一台
  │    └─ 拿到真实 IP（绕开了 VPN 的 DNS 劫持与 fake-IP）
  │
  ├─3 路由复核：tt_route_lookup_ifindex(resolvedIP) == ifIndex ?
  │    └─ 不等 → 内核仍会把包送进隧道 → 失败 BC_R_ROUTE_MISMATCH
  │
  ├─4 TCP：BCSocket::Create() → tt_socket_pin(fd, ifIndex, TCP) → connect(IP:443)
  │                              ↑ 必须在 connect 之前
  │
  ├─5 TLS：SNI = api.example.com，SSL_VERIFY_PEER，CA 校验 + X509_check_host
  │
  └─6 HTTP：GET /getServiceCallUrl + Host 头 → llhttp 解析 status/headers/body
```

第 3 步的路由复核是 `force-physical` 名副其实的关键。只绑网卡不够：Linux 上 `IP_UNICAST_IF` 只是软提示，拿不到 `CAP_NET_RAW` 时 `SO_BINDTODEVICE` 会失败，包照样进隧道。`tt_route_lookup_ifindex` 用内核路由表做 ground truth，做不到就明确失败，而不是假装成功、让服务端看到 VPN 出口 IP 并返回错误的接入点。

三档策略的差别就落在「哪一步失败允许回落」：

| 步骤 | `os` | `prefer-physical` | `force-physical` |
|---|---|---|---|
| 选网卡 | 不选，跟随系统 | 找不到物理网卡 → 回落隧道 | 失败 |
| DNS | `getaddrinfo` | 绑定/查询失败 → 回落 `getaddrinfo` | 失败 |
| TCP 绑定 | 不绑 | 失败 → 不绑继续 | 失败 |
| 路由复核 | 跳过 | 只告警 | 失败 |

`tt_route_lookup_ifindex` 在 iOS / Android 上是返回 0 的桩（见 `NetworkRouteLookup.h:15`）。返回 0 表示「不知道」，第 3 步在这两个平台上跳过，不判失败——沿用 `UDPSender` 既有约定。

### 八、HttpConnector API

```cpp
struct HttpRequest {
    std::string  method = "GET";
    std::string  url;
    HttpHeaderMap headers;
    std::string  body;
    uint32_t     timeoutMs = 10000;
    std::string  resolvedIp;      // 可选，非空则跳过 DNS
};

struct HttpResponse {
    int           status = 0;
    HttpHeaderMap headers;
    std::string   body;
    std::string   peerIp;         // 实际连上的对端 IP
    uint32_t      boundIfIndex;   // 实际绑定的网卡，0 = 未绑
    std::string   pinMethod;      // 实际生效的绑定手段
};

class IHttpRequestHandler {
public:
    virtual void OnHttpResponse(const HttpResponse& resp)                = 0;
    virtual void OnHttpError(BCRESULT result, const std::string& message) = 0;
};

class HttpConnector {
public:
    BCRESULT Create(BCFObject* pConfig, IHttpConnectorHandler* h);
    BCRESULT Request(const HttpRequest& req, IHttpRequestHandler* h);
    void     Close();
};
```

`HttpResponse` 带上 `peerIp` / `boundIfIndex` / `pinMethod` 是有意的：业务能据此判断这次请求到底有没有真的走物理网卡，而不用去翻日志。

**响应体大小上限**：默认 8 MB，超过即中断并返回 `BC_R_RESPONSE_TOO_LARGE`。防止畸形/恶意响应打爆内存。可通过 connector 配置调整。

**重定向**：不自动跟随。`3xx` 原样返回给调用方，`Location` 头在 `headers` 里。跟随重定向会让 `force-physical` 的语义变复杂（重定向目标需要重新解析、重新复核路由），交给业务显式发第二次请求更清晰。

### 九、配置项

`HttpConnector` 与 `WSConnector` 共用同一组 connector 级配置：

```
vpnPolicy           "os" | "prefer-physical" | "force-physical"
                    默认 tt_vpn_policy_platform_default()
bypassVpn           Boolean，deprecated，兼容映射同既有约定
dnsServers          ["223.5.5.5", "8.8.8.8"]，仅 prefer/force-physical 生效
dnsTimeoutMs        2000
caCerts             PEM 字符串，空则回落系统信任库
spkiPin             base64 SPKI pin
insecureSkipVerify  false，仅供自签调试
maxResponseBytes    8388608
```

`vpnPolicy` 的解析直接调用现成的 `tt_vpn_policy_resolve()`（含 `bypassVpn` 旧键兼容与 both-given 告警），不重写一套。

### 十、错误处理

失败统一回调 `(BCRESULT, errMessage)`。新增错误码沿用 `WSConnector.cpp:32-33` 已有的 `BC_R_NRESULTS + N` 惯例，集中定义在新增的 `src/cpp/TTErrors.h`，避免今天这种同一个 `BC_R_IDLE_TIMEOUT` 在 `WSConnector.cpp:32` 和 `WSServer.cpp:34` 各定义一次的重复：

| 错误码 | 值 | 含义 |
|---|---|---|
| `BC_R_IDLE_TIMEOUT` | `BC_R_NRESULTS + 1` | 沿用既有值，从 .cpp 提到 TTErrors.h |
| `BC_R_CONNECT_TIMEOUT` | `BC_R_NRESULTS + 2` | 同上 |
| `BC_R_NO_PHYSICAL_INTERFACE` | `BC_R_NRESULTS + 3` | 策略要求物理网卡但找不到 |
| `BC_R_PIN_FAILED` | `BC_R_NRESULTS + 4` | `tt_socket_pin` 在 force-physical 下失败 |
| `BC_R_ROUTE_MISMATCH` | `BC_R_NRESULTS + 5` | 路由复核发现内核仍走隧道 |
| `BC_R_DNS_FAILED` | `BC_R_NRESULTS + 6` | 所有 DNS server 均失败 |
| `BC_R_TLS_VERIFY_FAILED` | `BC_R_NRESULTS + 7` | 证书链/主机名/SPKI pin 校验失败 |
| `BC_R_RESPONSE_TOO_LARGE` | `BC_R_NRESULTS + 8` | 响应体超过 `maxResponseBytes` |

`errMessage` 必须说清楚卡在哪一步、为什么。`force-physical` 的失败信息尤其要具体——「`SO_BINDTODEVICE` 失败 errno=1，缺 CAP_NET_RAW」比「connect failed」有用得多。这条路径注定会在用户现场出问题，日志是唯一的排查手段。

不做自动降级重连：`force-physical` 失败后由业务决定是否改用 `prefer-physical` 重试。

## 测试

**单元测试**（沿用 `src/cpp/tests/` 的 standalone 约定，手动编译运行，不进构建目标）

- `DnsResolver_test.cpp` — 查询报文构造；响应解析含 name compression 指针、指针成环防御、越界偏移防御、`TC` 位、多 answer、CNAME 链
- `SocketPinner_test.cpp` — 参数校验、`ifIndex == 0` 返回 `NOT_NEEDED`、`outMethod` 填充
- 扩充 `VpnPolicy_test.cpp` — 覆盖新增的 TCP/DNS 场景组合

**端到端**

`tools/httpget` 命令行工具：

```
httpget --url https://example.com/path --vpn-policy force-physical \
        --dns 223.5.5.5 --log-level debug
```

打印 status / headers / body 前若干字节，以及 `peerIp` / `boundIfIndex` / `pinMethod`。配合 `certs/localhost.crt` 起本地 TLS server 验 https 与 wss。

**真实 VPN 场景（必须做，不可省略）**

开启 Clash/Surge 全局 TUN 模式，`tcpdump -i en0` 抓包确认：

1. DNS 查询报文从物理网卡发出，目标是配置的公共 DNS 而非 VPN 的 DNS
2. TCP SYN 从物理网卡发出，目标 IP 是真实 IP 而非 `198.18.x.x` 段
3. 服务端看到的 client IP 是真实 IP

同时验证 `os` 策略下流量正常走 VPN（即没有把 VPN 用户误伤成不能用）。这与 `22f7db9` 那次 force-physical 的抓包对照验证方式一致，验证记录同样补进 `docs/vpn-policy.md`。

## 跨项目影响

本轮只做 C++ 核心层，**不改动任何现有对外 API**，`rtc-client` / `livekit-ai` / `rtc-dashboard` 均无需同步修改。

下一轮接绑定层时会新增 API（而非修改），届时需要同步：

- `rtc-client` 的 `ttsignal.d.ts` — 新增 `createHttpConnector` / `request` 类型声明
- `src/java/org/difft/android/smp/` — 新增 `HttpConnector` / `HttpRequest` / `HttpResponse`
- `src/swift/` — 对应 Swift 类，与 Java 一对一对齐

## 实施顺序

1. `SocketPinner` 抽取 + `UDPSender` 改调用 + 单测。**先做这步且不改行为**，QUIC 侧回归通过再往下走
2. `TlsContext` 抽取（含 `SMPConnector` 改引用）+ 修既有两个 bug
3. `DnsResolver` + 单测
4. `TcpChannel`
5. `HttpConnector` + `tools/httpget`
6. `WSConnector` 改造接 `TcpChannel`，删 `WSServer` / `JNI_WS*`
7. `CMakeLists.txt` 三处 source list
8. 端到端 + 真实 VPN 抓包验证

第 1 步单独先行是有意的：它动的是已经上线、踩过坑的 QUIC 路径，必须与后续新功能隔离，出问题时能立刻定位是搬家搬坏了还是新代码的问题。
