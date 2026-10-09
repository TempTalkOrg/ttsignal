# iOS HTTP/WS binding Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让 iOS 侧能通过 Swift 用上已链入产物的 `HttpConnector` / `WSConnector`，能力面与 Node 对齐。

**Architecture:** 在 `ios_bridge.h/.mm` 增加两套 C-ABI（HTTP 用单次 completion，WS 用 vtable），沿用既有 opaque handle + userdata 风格；`src/swift/` 增加四个文件，HTTP 是 `async/await`、WS 是 delegate。所有新符号自动落入 `-exported_symbols_list` 的 `_tt_*` 白名单。

**Tech Stack:** Objective-C++ (`.mm`)、Swift 5.5+（`async/await` 需 back-deployment 到 iOS 13）、CMake、Xcode 16（QUICTest 用 `PBXFileSystemSynchronizedRootGroup`）。

**Spec:** [`docs/superpowers/specs/2026-09-07-ios-http-ws-binding-design.md`](../specs/2026-09-07-ios-http-ws-binding-design.md)

## Global Constraints

- **部署目标 iOS 13.0**（`IOS_DEPLOYMENT_TARGET` 默认值，见 `ios/scripts/build-deps.sh:34`）。
- **回调线程**：`IWSConnectionHandler` 的回调在该连接的 `TcpChannel` 事件循环线程；`IHttpRequestHandler` 的回调在工作线程。**都不是主线程**，Swift 文档必须写明调用方自行 hop 到 MainActor。
- **指针有效期**：所有回调参数里的指针只在回调期间有效，Swift 侧必须立刻拷贝。沿用既有 `on_recv_data` 约定。
- **错误码**：`int32_t` 是 `BCRESULT`，与 Node 同一套跨语言契约值。
- **符号命名**：新增导出一律 `tt_http_*` / `tt_ws_*` 前缀，否则会被 `build-xcframework.sh` 的符号防火墙降级为 private-extern。
- **不改动现有 C-ABI 与 Swift 类型**，纯增量。
- **Swift 文件同步**：写在 `src/swift/`，在 `ios/QUICTest/` 建**符号链接**（既有七个文件都是这么做的）；Xcode 16 的 file-system-synchronized group 会自动纳入编译，**不要改 `project.pbxproj`**。

### 验证方式说明（重要）

本仓库 iOS 侧没有 XCTest 基础设施，无法跑经典 TDD 红-绿循环。本计划的每个任务用这三级验证代替，与前几轮构建修复所用的手段一致：

1. **编译**：`ios/scripts/build-core.sh`（C-ABI）或 `xcodebuild`（Swift）。
2. **符号**：`nm` 确认导出面与防火墙完好。
3. **端到端**：Task 7 在模拟器实跑，拿真实运行结果。

每个任务的验证步骤都给出可直接粘贴的命令与预期输出。

---

### Task 1: 把 `ResultName` 提升为各绑定层共用

`ios_bridge.mm` 需要把 `BCRESULT` 转成 `"BC_R_ROUTE_MISMATCH"` 这种符号名填进 `errName`。该函数现在在 `src/cpp/napi/Utils.h`，而 iOS 目标的源码 glob 是 `${BASE_DIR}/cpp/*.cpp`，**不含 `napi/`**。`napi/Utils.h:60` 注释写明「新增错误码只改这里」，复制一份会直接破坏这个约定，因此移动而非复制。

**Files:**
- Modify: `src/cpp/Utils.h`（追加声明）
- Modify: `src/cpp/Utils.cpp`（迁入实现）
- Modify: `src/cpp/napi/Utils.h`（删声明，改为 include `Utils.h`）
- Modify: `src/cpp/napi/Utils.cpp`（删实现）

**Interfaces:**
- Consumes: 无
- Produces: `const char* ResultName(BCRESULT result);`，声明于 `src/cpp/Utils.h`，全局命名空间。表外的码返回 `"BC_R_(<数值>)"` 形式的静态缓冲字符串，**绝不退化成 `bc_result2string` 的散文**。

- [ ] **Step 1: 读现有实现，原样迁移**

```bash
sed -n '/ResultName/,/^}/p' src/cpp/napi/Utils.cpp
```

把整个函数体连同上方注释块原样搬到 `src/cpp/Utils.cpp` 末尾，声明搬到 `src/cpp/Utils.h`。注释里「新增错误码只改这里」那句保留。

- [ ] **Step 2: napi/Utils.h 改为转发**

删掉 `napi/Utils.h` 里的 `ResultName` 声明与其注释块，在文件顶部已有的 include 区加上：

```cpp
#include "Utils.h"   // ResultName —— 已提升为各绑定层共用（iOS bridge 也要用）
```

- [ ] **Step 3: 编译 Node 侧确认迁移没破坏**

```bash
bash build/macos-arm64-release/build 2>&1 | tail -3
```

预期：`[100%] Built target ttsignal`，无 `error:`。

- [ ] **Step 4: 全量四平台回归**

```bash
bash scripts/build-for-linux-and-macos.sh 2>&1 | tail -5
```

预期：`[build] all 4 addons built and verified clean.`，退出码 0。

- [ ] **Step 5: 提交**

```bash
git add src/cpp/Utils.h src/cpp/Utils.cpp src/cpp/napi/Utils.h src/cpp/napi/Utils.cpp
git commit -m "refactor(cpp): ResultName 提升到 src/cpp/Utils，供各绑定层共用

iOS 目标的源码 glob 只收 cpp/*.cpp，拿不到 napi/Utils.h 里的 ResultName，
而 ios_bridge 要用它填 errName。napi/Utils.h 注释写明「新增错误码只改这里」，
复制一份会让这句话失效，所以移动而非复制。"
```

---

### Task 2: HTTP C-ABI

**Files:**
- Modify: `src/cpp/apple/ios_bridge.h`（在 `tt_get_sdk_version` 声明之后、`} // extern "C"` 之前追加）
- Modify: `src/cpp/apple/ios_bridge.mm`（追加实现 + 顶部加 `#include "HttpConnector.h"`）

**Interfaces:**
- Consumes: `ResultName(BCRESULT)`（Task 1）
- Produces: `TTHttpConnectorRef` / `TTHttpConfig` / `TTHttpRequest` / `TTHttpResponse` / `TTHttpCompletion` / `TTCloseDone`；函数 `tt_http_connector_create` / `tt_http_request` / `tt_http_connector_close` / `tt_http_connector_destroy`。完整签名见 spec「C-ABI 设计 / HTTP」一节，**照抄，勿改字段顺序**。

- [ ] **Step 1: 在 ios_bridge.h 追加 HTTP 声明**

把 spec「C-ABI 设计 / HTTP」代码块整体粘进 `extern "C"` 块内，放在 `tt_get_sdk_version` 之后。给每个 struct 补一句用途注释，风格照 `TTConfig`（现有字段都有中文行注释）。

- [ ] **Step 2: ios_bridge.mm 顶部加 include**

```cpp
#include "HttpConnector.h"
```

放在现有 `#include "http-parser/http_parser.h"` 附近，保持既有分组。

- [ ] **Step 3: 实现 config 转换**

`HttpConnector::Create` 收 `BCFObject*`，键名照 `HttpConnector.cpp` 实际读取的那 11 个：`logLevel` `logFile` `vpnPolicy` `bypassVpn` `caCerts` `spkiPin` `insecureSkipVerify` `dnsServers` `dnsTimeoutMs` `drainTimeoutMs` `maxResponseBytes`。

```cpp
namespace {

// vpnPolicy 用 -1 表示未设置 —— 不写这个键，让原生层走平台默认。
// 与现有 ConvertConfig(TTConfig*) 的处理方式一致（ios_bridge.mm:123-131）。
static void FillNetConfig(BCFObject* p, int32_t vpnPolicy, const char* caCerts,
                          const char* spkiPin, int32_t insecureSkipVerify,
                          const char* dnsServers, uint32_t dnsTimeoutMs,
                          uint32_t drainTimeoutMs, int32_t logLevel)
{
    if (vpnPolicy >= 0) {
        p->PutString("vpnPolicy", tt_vpn_policy_to_string((TTVpnPolicy)vpnPolicy));
    }
    if (caCerts    && caCerts[0])    p->PutString("caCerts", caCerts);
    if (spkiPin    && spkiPin[0])    p->PutString("spkiPin", spkiPin);
    if (dnsServers && dnsServers[0]) p->PutString("dnsServers", dnsServers);
    if (insecureSkipVerify) p->PutBool("insecureSkipVerify", 1);
    if (dnsTimeoutMs)       p->PutInt("dnsTimeoutMs",   (int)dnsTimeoutMs);
    if (drainTimeoutMs)     p->PutInt("drainTimeoutMs", (int)drainTimeoutMs);
    if (logLevel)           p->PutInt("logLevel",       (int)logLevel);
}

} // namespace
```

- [ ] **Step 4: 实现 connector 包装与 handler proxy**

```cpp
namespace {

// HttpConnector::Create 要一个 IHttpConnectorHandler。日志走 BC 的 appender，
// 这里只需要一个空实现 + close 完成信号。
class TTHttpConnectorProxy : public IHttpConnectorHandler {
public:
    TTCloseDone closeDone = nullptr;
    void*       closeUd   = nullptr;
    void OnLog(int, LPCSTR) override {}
    void OnClosed() override {
        if (closeDone) { closeDone(closeUd); closeDone = nullptr; }
    }
};

} // namespace

struct TTHttpConnector {
    HttpConnector*        connector = nullptr;
    TTHttpConnectorProxy* proxy     = nullptr;
};
```

- [ ] **Step 5: 实现请求 handler —— 响应组装是本任务的核心**

`HttpResponse.headers` 是 `HttpHeaderMap`（键统一小写、同名头以 `", "` 合并）。平行数组的 `c_str()` 指针在 `resp` 存活期间有效，而 completion 是在回调内同步调用的，因此安全。

```cpp
namespace {

class TTHttpRequestCtx : public IHttpRequestHandler {
public:
    TTHttpCompletion completion = nullptr;
    void*            userdata   = nullptr;

    void OnHttpResponse(const HttpResponse& resp) override {
        std::vector<const char*> names, values;
        names.reserve(resp.headers.size());
        values.reserve(resp.headers.size());
        for (const auto& kv : resp.headers) {
            names.push_back(kv.first.c_str());
            values.push_back(kv.second.c_str());
        }
        TTHttpResponse out;
        memset(&out, 0, sizeof(out));
        out.status       = resp.status;
        out.reason       = resp.reason.c_str();
        out.headerNames  = names.empty()  ? nullptr : names.data();
        out.headerValues = values.empty() ? nullptr : values.data();
        out.headerCount  = names.size();
        out.body         = resp.body.empty() ? nullptr
                                             : (const uint8_t*)resp.body.data();
        out.bodyLen      = resp.body.size();
        out.peerIp       = resp.peerIp.c_str();
        out.boundIfIndex = resp.boundIfIndex;
        out.pinMethod    = resp.pinMethod.c_str();
        if (completion) completion(userdata, 0, nullptr, nullptr, &out);
        delete this;
    }

    void OnHttpError(BCRESULT result, const std::string& message) override {
        if (completion) {
            completion(userdata, (int32_t)result,
                       ResultName(result), message.c_str(), nullptr);
        }
        delete this;
    }
};

} // namespace
```

- [ ] **Step 6: 实现四个导出函数**

```cpp
TTHttpConnectorRef tt_http_connector_create(const TTHttpConfig* config)
{
    if (!config) return nullptr;
    BCFObject cfg;
    FillNetConfig(&cfg, config->vpnPolicy, config->caCerts, config->spkiPin,
                  config->insecureSkipVerify, config->dnsServers,
                  config->dnsTimeoutMs, config->drainTimeoutMs, config->logLevel);
    if (config->maxResponseBytes) {
        cfg.PutInt("maxResponseBytes", (int)config->maxResponseBytes);
    }

    TTHttpConnector* self = new TTHttpConnector();
    self->proxy     = new TTHttpConnectorProxy();
    self->connector = new HttpConnector();
    if (self->connector->Create(&cfg, self->proxy) != BC_R_SUCCESS) {
        delete self->connector; delete self->proxy; delete self;
        return nullptr;
    }
    return self;
}

int32_t tt_http_request(TTHttpConnectorRef cref, const TTHttpRequest* req,
                        TTHttpCompletion completion, void* userdata)
{
    if (!cref || !req || !req->url || !completion) return BC_R_INVALIDARG;

    HttpRequest r;
    if (req->method && req->method[0]) r.method = req->method;
    r.url = req->url;
    for (size_t i = 0; i < req->headerCount; ++i) {
        if (req->headerNames[i] && req->headerValues[i]) {
            r.headers[req->headerNames[i]] = req->headerValues[i];
        }
    }
    if (req->body && req->bodyLen) {
        r.body.assign((const char*)req->body, req->bodyLen);
    }
    if (req->timeoutMs)  r.timeoutMs  = req->timeoutMs;
    if (req->resolvedIp) r.resolvedIp = req->resolvedIp;

    TTHttpRequestCtx* ctx = new TTHttpRequestCtx();
    ctx->completion = completion;
    ctx->userdata   = userdata;
    BCRESULT rc = cref->connector->Request(r, ctx);
    if (rc != BC_R_SUCCESS) {
        delete ctx;          // 没受理 => completion 不会来，ctx 自己回收
        return (int32_t)rc;
    }
    return 0;
}

void tt_http_connector_close(TTHttpConnectorRef cref, TTCloseDone done, void* ud)
{
    if (!cref) { if (done) done(ud); return; }
    cref->proxy->closeDone = done;
    cref->proxy->closeUd   = ud;
    cref->connector->Close();
}

void tt_http_connector_destroy(TTHttpConnectorRef cref)
{
    if (!cref) return;
    delete cref->connector;
    delete cref->proxy;
    delete cref;
}
```

- [ ] **Step 7: 编译三 slice**

```bash
bash ios/scripts/build-core.sh 2>&1 | grep -cE "error:"
```

预期：`0`。若非 0，看完整日志定位。

- [ ] **Step 8: 符号验证**

```bash
nm build/ios-device-arm64-release/dist/lib/libttsignal_ios.a 2>/dev/null \
  | grep -cE "_tt_http_(connector_create|request|connector_close|connector_destroy)$"
```

预期：`4`。

- [ ] **Step 9: 提交**

```bash
git add src/cpp/apple/ios_bridge.h src/cpp/apple/ios_bridge.mm
git commit -m "feat(ios): ios_bridge 导出 HTTP C-ABI

一次性请求用单个 completion，响应用扁平 struct + headers 平行数组传回，
指针仅回调内有效（沿用 on_recv_data 约定）。errName 走 Task 1 提升出来的
ResultName，errMessage 原样透传原生现场描述 —— force-physical 的排查只能
看那一段。"
```

---

### Task 3: WS C-ABI

**Files:**
- Modify: `src/cpp/apple/ios_bridge.h`（追加 WS 声明）
- Modify: `src/cpp/apple/ios_bridge.mm`（追加实现 + `#include "WSConnector.h"`）

**Interfaces:**
- Consumes: `ResultName`（Task 1）、`FillNetConfig`（Task 2 的 file-scope helper）
- Produces: `TTWSConnectorRef` / `TTWSConnectionRef` / `TTWSConfig` / `TTWSHandlerVTable`；函数 `tt_ws_connector_create` / `tt_ws_connector_create_connection` / `tt_ws_connector_close` / `tt_ws_connector_destroy` / `tt_ws_connection_connect` / `tt_ws_connection_set_request_header` / `tt_ws_connection_send_text` / `tt_ws_connection_send_data` / `tt_ws_connection_send_ping` / `tt_ws_connection_bound_if` / `tt_ws_connection_close` / `tt_ws_connection_destroy`

- [ ] **Step 1: 在 ios_bridge.h 追加 WS 声明**

照抄 spec「C-ABI 设计 / WS」代码块。`on_release` 的注释必须保留——它是整个生命周期设计的关键。

- [ ] **Step 2: 实现 adapter —— 本计划最易出错的一处**

`WSConnector.h` 文件顶部的契约：`Connect()` 返回非成功时无任何回调；返回成功后 `OnConnectResult` 恰好一次，**且仅在 result 成功时**之后才有 `OnClosed`。据此收敛释放判定：

```cpp
namespace {

class TTWSConnAdapter : public IWSConnectionHandler {
public:
    WSConnPtr         conn;
    TTWSHandlerVTable vt;
    void*             userdata = nullptr;
    std::atomic<bool> released{false};

    // 契约保证每条连接恰好一次；用 CAS 兜住并发与重复路径。
    void ReleaseOnce() {
        bool expected = false;
        if (released.compare_exchange_strong(expected, true)) {
            if (vt.on_release) vt.on_release(userdata);
        }
    }

    void OnConnectResult(BCRESULT result, const HttpHeaderMap&) override {
        // LastErrorMessage() 的值在派发回调之前就写好了，回调里一定读得到；
        // 任意线程可调。WSConnector.h:236-241 明确要求绑定层这样透传。
        std::string msg = conn ? conn->LastErrorMessage() : std::string();
        if (vt.on_connect_result) {
            vt.on_connect_result(userdata, (int32_t)result, msg.c_str());
        }
        // result != SUCCESS 时不再有 OnClosed（契约 2），必须就地释放，
        // 否则 box 永久泄漏。
        if (result != BC_R_SUCCESS) ReleaseOnce();
    }

    void OnRecvText(LPCSTR t) override {
        if (vt.on_text && t) vt.on_text(userdata, t, strlen(t));
    }
    void OnRecvData(LPCVOID d, size_t n) override {
        if (vt.on_data) vt.on_data(userdata, (const uint8_t*)d, n);
    }
    void OnClosed(LPCSTR reason) override {
        if (vt.on_closed) vt.on_closed(userdata, reason);
        ReleaseOnce();
    }
    void OnException(BCException& e) override {
        if (vt.on_exception) vt.on_exception(userdata, e.what());
    }
};

} // namespace

struct TTWSConnector  { WSConnector* connector = nullptr; /* + proxy */ };
struct TTWSConnection { TTWSConnAdapter* adapter = nullptr; };
```

- [ ] **Step 3: 实现 `tt_ws_connector_create`，写全配置键**

`WSConnector::Create` 收 `BCFObject*`。除 `FillNetConfig`（Task 2）覆盖的 8 项公共键外，还要写 WS 专有的 7 项。键名照 `WSConnector.cpp` 实际读取的那份（`grep -oE 'Get\("[a-zA-Z_]+"\)' src/cpp/WSConnector.cpp`）：

```cpp
TTWSConnectorRef tt_ws_connector_create(const TTWSConfig* config)
{
    if (!config) return nullptr;
    BCFObject cfg;
    FillNetConfig(&cfg, config->vpnPolicy, config->caCerts, config->spkiPin,
                  config->insecureSkipVerify, config->dnsServers,
                  config->dnsTimeoutMs, config->drainTimeoutMs, config->logLevel);
    // WS 专有。0 / 空串一律不写键，让原生层用自己的默认值。
    if (config->connectTimeoutMs) cfg.PutInt("connectTimeoutMs", (int)config->connectTimeoutMs);
    if (config->pingIntervalMs)   cfg.PutInt("pingIntervalMs",   (int)config->pingIntervalMs);
    if (config->idleTimeoutMs)    cfg.PutInt("idleTimeoutMs",    (int)config->idleTimeoutMs);
    if (config->maxFrameBytes)    cfg.PutInt("maxFrameBytes",    (int)config->maxFrameBytes);
    if (config->clientCertFile && config->clientCertFile[0])
        cfg.PutString("clientCertFile", config->clientCertFile);
    if (config->clientKeyFile && config->clientKeyFile[0])
        cfg.PutString("clientKeyFile", config->clientKeyFile);
    if (config->clientKeyPassword && config->clientKeyPassword[0])
        cfg.PutString("clientKeyPassword", config->clientKeyPassword);

    TTWSConnector* self = new TTWSConnector();
    self->proxy     = new TTWSConnectorProxy();   // IWSConnectorHandler 空实现 + close 信号
    self->connector = new WSConnector();
    if (self->connector->Create(&cfg, self->proxy) != BC_R_SUCCESS) {
        delete self->connector; delete self->proxy; delete self;
        return nullptr;
    }
    return self;
}
```

`TTWSConnectorProxy` 照 Task 2 的 `TTHttpConnectorProxy` 写：实现 `IWSConnectorHandler`，`OnLog` 空实现，`OnClosed` 里回调 `TTCloseDone` 并置空。

`tt_ws_connector_create_connection` 的 per-connection config 传 `nullptr`——连接级逐键覆盖不在本次范围（Node 侧虽支持，但 iOS 尚无消费方；`WSConnector.h:471` 注释写明 `pConfig` 可为 NULL 时沿用连接器默认配置）。

- [ ] **Step 4: 实现连接工厂与生命周期函数**

`destroy` 必须兜住「从未 connect 就销毁」的情形——那条路径上不会有任何回调，`on_release` 只能由 destroy 补发：

```cpp
TTWSConnectionRef tt_ws_connector_create_connection(TTWSConnectorRef cref,
        const TTWSHandlerVTable* vtable, void* userdata)
{
    if (!cref || !vtable) return nullptr;
    TTWSConnAdapter* ad = new TTWSConnAdapter();
    ad->vt       = *vtable;
    ad->userdata = userdata;
    ad->conn     = cref->connector->CreateConnection(nullptr, ad);
    if (!ad->conn) { delete ad; return nullptr; }
    TTWSConnection* self = new TTWSConnection();
    self->adapter = ad;
    return self;
}

int32_t tt_ws_connection_connect(TTWSConnectionRef c, const char* url, uint32_t t)
{
    if (!c || !url) return BC_R_INVALIDARG;
    BCRESULT rc = c->adapter->conn->Connect(url, t);
    // 契约 1：非成功返回时不会有任何回调 —— 就地释放，否则泄漏。
    if (rc != BC_R_SUCCESS) c->adapter->ReleaseOnce();
    return (int32_t)rc;
}

void tt_ws_connection_destroy(TTWSConnectionRef c)
{
    if (!c) return;
    // 从未 connect / connect 失败后销毁：补发一次，CAS 保证不会重复。
    c->adapter->ReleaseOnce();
    delete c->adapter;   // WSConnPtr 是 shared_ptr，连接器仍持有直到收尾
    delete c;
}
```

其余 `send_text` / `send_data` / `send_ping` / `set_request_header` / `bound_if` / `close` 是直接转发，各自 NULL 检查后调同名 `WSConnection` 方法，返回 `(int32_t)` 结果。

- [ ] **Step 5: 编译三 slice**

```bash
bash ios/scripts/build-core.sh 2>&1 | grep -cE "error:"
```

预期：`0`。

- [ ] **Step 6: 符号验证**

```bash
nm build/ios-device-arm64-release/dist/lib/libttsignal_ios.a 2>/dev/null \
  | grep -cE "_tt_ws_[a-z_]+$"
```

预期：`12`。

- [ ] **Step 7: 提交**

```bash
git add src/cpp/apple/ios_bridge.h src/cpp/apple/ios_bridge.mm
git commit -m "feat(ios): ios_bridge 导出 WS C-ABI

vtable 加 on_release：WSConnector.h 契约规定 connect 失败后不再有 OnClosed，
无脑等 OnClosed 释放 box 就是泄漏。释放判定收敛在 adapter 一侧（CAS 保证
恰好一次），Swift 不必理解契约细节。connect 结果的 message 走
LastErrorMessage() 原样透传，而不是像 Node 那样按错误码套硬编码文案。"
```

---

### Task 4: Swift 基础层（NetConfig + Error）

**Files:**
- Create: `src/swift/TTSignalNetConfig.swift`
- Create: `src/swift/TTSignalError.swift`
- Create: `ios/QUICTest/TTSignalNetConfig.swift`（符号链接）
- Create: `ios/QUICTest/TTSignalError.swift`（符号链接）

**Interfaces:**
- Consumes: Task 2/3 的 C 结构体
- Produces: `struct TTSignalNetConfig`（字段 `vpnPolicy: TTSignalVPNPolicy?` / `caCerts: String` / `spkiPin: String` / `insecureSkipVerify: Bool` / `dnsServers: String` / `dnsTimeoutMs: UInt32` / `drainTimeoutMs: UInt32` / `logLevel: Int32`）；`struct TTSignalError: Error`（`result: Int32` / `errName: String?` / `errMessage: String?`）

- [ ] **Step 1: 写 TTSignalError.swift**

```swift
///////////////////////////////////////////////////////////////////////////////
// file : TTSignalError.swift
// author : anto
//
// HTTP/WS 绑定层的错误类型。三个字段原样透传原生层的值，不重新包装 ——
// 与 Node 侧 (src/js/index.js) 同一约定。
///////////////////////////////////////////////////////////////////////////////

import Foundation

public struct TTSignalError: Error {
    /// BCRESULT 数值，跨语言契约值。常见取值见 errName。
    public let result: Int32
    /// 错误码符号名，如 "BC_R_ROUTE_MISMATCH"。
    public let errName: String?
    /// 原生层的现场描述。**排查 force-physical 只能看这一段** —— 它包含对端
    /// IP、peerClass、内核把包判给了哪块网卡、以及该怎么改配置。通用提示给不
    /// 出这些细节，所以绝不要吞掉或改写它。
    ///
    /// ⚠️ 发送类接口（sendText / sendData / sendPing）抛出的错误只有 result，
    /// errName / errMessage 都是 nil —— 原生发送接口只给错误码。
    public let errMessage: String?

    public init(result: Int32, errName: String? = nil, errMessage: String? = nil) {
        self.result = result
        self.errName = errName
        self.errMessage = errMessage
    }
}

extension TTSignalError: CustomStringConvertible {
    public var description: String {
        "TTSignalError(\(result) \(errName ?? "?")): \(errMessage ?? "")"
    }
}
```

- [ ] **Step 2: 写 TTSignalNetConfig.swift**

字段与 spec 一致。`vpnPolicy` 是 `Optional` —— `nil` 时向 C 侧写 `-1`，让原生走平台默认，与现有 `TTSignalConfig` 的处理一致。

- [ ] **Step 3: 建符号链接**

```bash
cd ios/QUICTest
ln -s ../../src/swift/TTSignalNetConfig.swift TTSignalNetConfig.swift
ln -s ../../src/swift/TTSignalError.swift     TTSignalError.swift
ls -la TTSignalNetConfig.swift TTSignalError.swift
```

预期：两行都是 `->` 指向 `../../src/swift/`。**不要改 `project.pbxproj`**，Xcode 16 的 file-system-synchronized group 会自动收进来。

- [ ] **Step 4: 编译验证**

```bash
bash ios/scripts/build-quictest.sh > /tmp/qt.log 2>&1; echo "EXIT=$?"
xcodebuild -project ios/QUICTest.xcodeproj -scheme QUICTest \
  -sdk iphonesimulator -destination 'platform=iOS Simulator,name=iPhone 16' \
  build 2>&1 | tail -5
```

预期：`** BUILD SUCCEEDED **`。

- [ ] **Step 5: 提交**

```bash
git add src/swift/TTSignalNetConfig.swift src/swift/TTSignalError.swift \
        ios/QUICTest/TTSignalNetConfig.swift ios/QUICTest/TTSignalError.swift
git commit -m "feat(ios): Swift 侧 TTSignalNetConfig 与 TTSignalError

配置公共部分单独成型，HTTP/WS 各自内嵌，避免复用 TTSignalConfig 时
在公共 API 上摆一堆对 HTTP/WS 无效的字段。"
```

---

### Task 5: Swift HTTP（async/await）

**Files:**
- Create: `src/swift/TTSignalHttp.swift`
- Create: `ios/QUICTest/TTSignalHttp.swift`（符号链接）

**Interfaces:**
- Consumes: `TTSignalNetConfig` / `TTSignalError`（Task 4）、`tt_http_*`（Task 2）
- Produces: `struct TTSignalHttpConfig`（`net: TTSignalNetConfig` / `maxResponseBytes: Int`）；`struct TTSignalHttpResponse`（`status: Int32` / `reason: String` / `headers: [String: String]` / `body: Data` / `peerIp: String` / `boundIfIndex: UInt32` / `pinMethod: String`，便利属性 `bodyText: String?`）；`final class TTSignalHttpConnector`，方法 `init?(config:)` / `request(method:url:headers:body:timeoutMs:resolvedIp:) async throws -> TTSignalHttpResponse` / `close() async`

- [ ] **Step 1: 写 box 与 completion 蹦床**

continuation 只能 resume 一次，靠 C++ 契约「completion 恰好一次」保证。`tt_http_request` 返回非 0 表示没受理、completion 不会来，此时必须自己释放 box 并 resume，否则调用方永久挂起 + box 泄漏：

```swift
private final class HttpBox {
    let cont: CheckedContinuation<TTSignalHttpResponse, Error>
    init(_ c: CheckedContinuation<TTSignalHttpResponse, Error>) { cont = c }
}

private let httpCompletion: TTHttpCompletion = { ud, error, errName, errMessage, resp in
    guard let ud else { return }
    let box = Unmanaged<HttpBox>.fromOpaque(ud).takeRetainedValue()
    if error != 0 || resp == nil {
        box.cont.resume(throwing: TTSignalError(
            result: error,
            errName:    errName.map    { String(cString: $0) },
            errMessage: errMessage.map { String(cString: $0) }))
        return
    }
    box.cont.resume(returning: TTSignalHttpResponse(c: resp!.pointee))
}
```

- [ ] **Step 2: 写响应组装**

headers 从平行数组组装。**键已由原生层统一转小写**，且同名头以 `", "` 合并（RFC 7230 3.2.2）——这会拼坏 `Set-Cookie`，本模块定位是 JSON API 客户端，文档注释里要写明这一点。

```swift
extension TTSignalHttpResponse {
    init(c: TTHttpResponse) {
        status = c.status
        reason = c.reason.map { String(cString: $0) } ?? ""
        var h = [String: String]()
        if let names = c.headerNames, let values = c.headerValues {
            for i in 0..<c.headerCount {
                guard let n = names[i], let v = values[i] else { continue }
                h[String(cString: n)] = String(cString: v)
            }
        }
        headers = h
        body = (c.body != nil && c.bodyLen > 0)
             ? Data(bytes: c.body!, count: c.bodyLen) : Data()
        peerIp       = c.peerIp.map { String(cString: $0) } ?? ""
        boundIfIndex = c.boundIfIndex
        pinMethod    = c.pinMethod.map { String(cString: $0) } ?? ""
    }
    public var bodyText: String? { String(data: body, encoding: .utf8) }
}
```

- [ ] **Step 3: 写 request 方法**

headers 要转成 C 的平行数组，且这些 `strdup` 出来的指针必须活到 `tt_http_request` 返回（原生层在调用内拷贝进 `HttpRequest`），之后立刻释放。

- [ ] **Step 4: 文档注释写明线程约束**

在 `request` 的注释里写明：completion 在**工作线程**上 resume continuation，`await` 之后的代码会继续在该线程跑一段，**不要在那里做耗时操作**——与 Node 侧 `index.js` 注释里那段同一件事。

- [ ] **Step 5: 建符号链接并编译**

```bash
ln -s ../../src/swift/TTSignalHttp.swift ios/QUICTest/TTSignalHttp.swift
xcodebuild -project ios/QUICTest.xcodeproj -scheme QUICTest \
  -sdk iphonesimulator -destination 'platform=iOS Simulator,name=iPhone 16' \
  build 2>&1 | tail -5
```

预期：`** BUILD SUCCEEDED **`。

- [ ] **Step 6: 提交**

```bash
git add src/swift/TTSignalHttp.swift ios/QUICTest/TTSignalHttp.swift
git commit -m "feat(ios): Swift HTTP binding，async/await 形态"
```

---

### Task 6: Swift WS（delegate）

**Files:**
- Create: `src/swift/TTSignalWS.swift`
- Create: `ios/QUICTest/TTSignalWS.swift`（符号链接）

**Interfaces:**
- Consumes: `TTSignalNetConfig` / `TTSignalError`（Task 4）、`tt_ws_*`（Task 3）
- Produces: `struct TTSignalWSConfig`；`protocol TTSignalWSHandler: AnyObject`（`onConnectResult(_:error:message:)` / `onText(_:text:)` / `onData(_:data:)` / `onClosed(_:reason:)` / `onException(_:errMsg:)`）；`final class TTSignalWSConnector`（`init?(config:)` / `createConnection(handler:) -> TTSignalWSConnection?` / `close()`）；`final class TTSignalWSConnection`（`connect(url:timeoutMs:) throws` / `setRequestHeader(_:_:) throws` / `sendText(_:) throws` / `sendData(_:) throws` / `sendPing() throws` / `close()` / `boundIfIndex: UInt32`）

- [ ] **Step 1: 写 vtable 静态实例**

box 的释放**只在 `on_release` 里做**，其余回调一律 `takeUnretainedValue`。这是 Task 3 那套契约在 Swift 侧的对应面：

```swift
private final class WSBox {
    weak var conn: TTSignalWSConnection?
    let handler: TTSignalWSHandler
    init(handler: TTSignalWSHandler) { self.handler = handler }
}

private func wsBox(_ ud: UnsafeMutableRawPointer?) -> WSBox? {
    guard let ud else { return nil }
    return Unmanaged<WSBox>.fromOpaque(ud).takeUnretainedValue()
}

private var wsVTable = TTWSHandlerVTable(
    on_connect_result: { ud, error, message in
        guard let b = wsBox(ud), let c = b.conn else { return }
        b.handler.onConnectResult(c, error: error,
                                  message: message.map { String(cString: $0) })
    },
    on_text: { ud, text, len in
        guard let b = wsBox(ud), let c = b.conn, let text else { return }
        b.handler.onText(c, text: String(cString: text))
    },
    on_data: { ud, data, len in
        guard let b = wsBox(ud), let c = b.conn, let data else { return }
        b.handler.onData(c, data: Data(bytes: data, count: len))
    },
    on_closed: { ud, reason in
        guard let b = wsBox(ud), let c = b.conn else { return }
        b.handler.onClosed(c, reason: reason.map { String(cString: $0) })
    },
    on_exception: { ud, msg in
        guard let b = wsBox(ud), let c = b.conn, let msg else { return }
        b.handler.onException(c, errMsg: String(cString: msg))
    },
    // 唯一释放点。bridge 依据 WSConnector.h 契约保证恰好一次。
    on_release: { ud in
        guard let ud else { return }
        Unmanaged<WSBox>.fromOpaque(ud).release()
    }
)
```

- [ ] **Step 2: 写 connector 与 connection 类**

`TTSignalWSConnection.deinit` 调 `tt_ws_connection_destroy`；protocol extension 给 `onException` 与 `onConnectResult` 之外的方法默认空实现，与现有 `TTSignalHandler` 的做法一致。

- [ ] **Step 3: 文档注释写明回调线程**

照 `TTSignalHandler.swift` 顶部那段：「All callbacks are dispatched on internal worker threads — implementations must hop to MainActor / DispatchQueue.main themselves before touching UI.」

- [ ] **Step 4: 建符号链接并编译**

```bash
ln -s ../../src/swift/TTSignalWS.swift ios/QUICTest/TTSignalWS.swift
xcodebuild -project ios/QUICTest.xcodeproj -scheme QUICTest \
  -sdk iphonesimulator -destination 'platform=iOS Simulator,name=iPhone 16' \
  build 2>&1 | tail -5
```

预期：`** BUILD SUCCEEDED **`。

- [ ] **Step 5: 提交**

```bash
git add src/swift/TTSignalWS.swift ios/QUICTest/TTSignalWS.swift
git commit -m "feat(ios): Swift WS binding，delegate 形态

box 只在 on_release 里释放 —— 那是 bridge 依据 WSConnector.h 契约
判定的唯一释放点，其余回调一律 takeUnretainedValue。"
```

---

### Task 7: QUICTest 验证入口 + 模拟器实跑

**Files:**
- Create: `ios/QUICTest/HttpWSDemo.swift`
- Modify: `ios/QUICTest/ContentView.swift`（加入口按钮）

**Interfaces:**
- Consumes: Task 4/5/6 的全部 Swift 类型
- Produces: 无（终端验证）

- [ ] **Step 1: 写验证视图**

四项验证，对应 spec「验证方案」：

```swift
// 1) HTTP + vpnPolicy 对照
for policy in [TTSignalVPNPolicy.os, .forcePhysical] {
    var cfg = TTSignalHttpConfig()
    cfg.net.vpnPolicy = policy
    guard let http = TTSignalHttpConnector(config: cfg) else { continue }
    do {
        let r = try await http.request(url: "https://ipinfo.io/json")
        log("\(policy): status=\(r.status) ip=\(r.bodyText ?? "") " +
            "boundIf=\(r.boundIfIndex) pin=\(r.pinMethod)")
    } catch let e as TTSignalError {
        log("\(policy) FAILED: \(e.result) \(e.errName ?? "") \(e.errMessage ?? "")")
    }
    await http.close()
}
```

- [ ] **Step 2: 加错误路径与 WS 收发**

错误路径请求一个必然失败的地址，确认 `TTSignalError` 三个字段都非空；WS 连公开 echo 服务，`sendText` 后确认 `onText` 收到相同内容，`close()` 后确认 `onClosed` 到达。

- [ ] **Step 3: 加 on_release 计数断言**

临时在 `on_release` 里打日志，分别跑「连接失败」与「正常关闭」两条路径，各确认恰好一次。**验证通过后移除该日志**。

- [ ] **Step 4: 模拟器实跑**

```bash
xcrun simctl boot "iPhone 16" 2>/dev/null || true
xcodebuild -project ios/QUICTest.xcodeproj -scheme QUICTest \
  -sdk iphonesimulator -destination 'platform=iOS Simulator,name=iPhone 16' \
  build 2>&1 | tail -3
xcrun simctl install booted "$(find ~/Library/Developer/Xcode/DerivedData \
  -name 'QUICTest.app' -path '*Simulator*' | head -1)"
xcrun simctl launch --console booted com.chative.QUICTest
```

预期输出：`os` 那次拿到系统默认路由的出口 IP；`forcePhysical` 那次拿到物理网卡真实 IP，且 `boundIf != 0`、`pin=IP_BOUND_IF`。对照第一轮 Node 侧结果（`os` 走 VPN 出口，`force-physical` 走物理网卡真实出口）。

- [ ] **Step 5: 符号防火墙复核**

```bash
bash ios/scripts/build-all.sh > /tmp/ios.log 2>&1; echo "EXIT=$?"
A=build/ios-xcframework/TTSignal.xcframework/ios-arm64/libTTSignal.a
nm "$A" | awk '$2=="T"{print $3}' | grep -vc "^_tt_"
```

预期：`EXIT=0`，且最后一条输出 `0`——除 `_tt_*` 外没有新增外部可见符号。

- [ ] **Step 6: 提交**

```bash
git add ios/QUICTest/HttpWSDemo.swift ios/QUICTest/ContentView.swift
git commit -m "test(ios): QUICTest 加 HTTP/WS 验证入口

对照 os / force-physical 两种策略的出口 IP，验证 iOS 上也能绕开 VPN
拿到用户真实 IP；覆盖错误路径与 WS 收发，并确认 on_release 恰好一次。"
```

---

## 自查记录

**Spec 覆盖**：C-ABI（Task 2/3）、Swift 四文件（Task 4/5/6，`TTSignalHttp.swift` 同时承载 `TTSignalHttpConfig`/`Response`/`Connector`）、生命周期与线程模型（Task 3 Step 2、Task 5 Step 1/4、Task 6 Step 1/3）、验证方案四项（Task 7 Step 1-3、Step 5）、构建集成（Task 2 Step 2 的 include、Task 4 Step 3 的符号链接）。`ResultName` 的可用性是 spec 未点出的隐含前提，补为 Task 1。

**类型一致性**：`TTCloseDone` 在 HTTP 与 WS 共用（spec 已统一命名）；`FillNetConfig` 由 Task 2 定义、Task 3 消费，已在 Interfaces 标注；Swift 侧 `TTSignalNetConfig` 由 Task 4 产出，Task 5/6 消费。


