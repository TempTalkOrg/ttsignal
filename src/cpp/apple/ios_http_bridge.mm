///////////////////////////////////////////////////////////////////////////////
// file : ios_http_bridge.mm
// author : anto
//
// extern "C" Swift-facing surface for the HTTP / WebSocket stack
// (HttpConnector / WSConnector). The iOS counterpart of
// src/cpp/napi/JsHttpConnectorWrap.cpp + JsWSConnectorWrap.cpp.
//
// ⚠️ 为什么单独一个文件，而不是并进 ios_bridge.mm：
// llhttp.h（HttpConnector.h 间接带进来）与 http-parser/http_parser.h
// （ios_bridge.mm 用它解析 URL）**定义了同名的 HPE_* 枚举**，放进同一个翻译
// 单元会得到二十来条 "redefinition of enumerator 'HPE_OK'"。两边都是既有代码
// 且各有用途，拆文件是唯一不改动现有实现的解法。
//
// 这条栈与 ios_bridge.mm 那条 SMP/QUIC 栈相互独立：不共用 connector，也不共用
// 配置键。用途是在 VPN / 虚拟网卡环境下用**用户真实 IP** 出网。
///////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "ios_bridge.h"
#include "BC/BCFCodec.h"
#include "Utils.h"          // node::ResultName
#include "VpnPolicy.h"      // tt_vpn_policy_to_string
#include "HttpConnector.h"
#include "Runtime.h"
#include "WSConnector.h"

#include <string>
#include <vector>
#include <cstring>
#include <mutex>
#include <atomic>

#import <Foundation/Foundation.h>

namespace ios_http_bridge {

///////////////////////////////////////////////////////////////////////////////
// HTTP / WS 共用的配置搬运
//
// 这条栈（TcpChannel 系）与上面 ConvertConfig 服务的 SMP/QUIC 栈读的是**两套
// 不同的键**，所以不能复用 ConvertConfig。键名以 HttpConnector.cpp /
// WSConnector.cpp 里实际 Get("...") 的那些为准。
//
// 0 / 空串 / vpnPolicy<0 一律**不写键**，让原生层用自己的默认值 —— 与
// ConvertConfig 对 vpnPolicy 的处理同一思路。
///////////////////////////////////////////////////////////////////////////////

static void FillNetConfig(BCFObject* p, int32_t vpnPolicy, const char* caCerts,
                          const char* spkiPin, int32_t insecureSkipVerify,
                          const char* dnsServers, uint32_t dnsTimeoutMs,
                          uint32_t drainTimeoutMs, int32_t logLevel)
{
    if (!p) return;
    if (vpnPolicy >= 0) {
        p->PutString("vpnPolicy",
                     tt_vpn_policy_to_string((TTVpnPolicy)vpnPolicy));
    }
    if (caCerts    && caCerts[0])    p->PutString("caCerts",    caCerts);
    if (spkiPin    && spkiPin[0])    p->PutString("spkiPin",    spkiPin);
    if (dnsServers && dnsServers[0]) p->PutString("dnsServers", dnsServers);
    if (insecureSkipVerify)          p->PutBool("insecureSkipVerify", 1);
    if (dnsTimeoutMs)   p->PutInt("dnsTimeoutMs",   (int)dnsTimeoutMs);
    if (drainTimeoutMs) p->PutInt("drainTimeoutMs", (int)drainTimeoutMs);
    if (logLevel)       p->PutInt("logLevel",       (int)logLevel);
}

///////////////////////////////////////////////////////////////////////////////
// HttpConnector::Create 要一个 IHttpConnectorHandler。日志已经由 BC 的 appender
// 接管（FillNetConfig 写了 logLevel），这里只需要接住 OnClosed 转成 C 回调。
///////////////////////////////////////////////////////////////////////////////

class TTHttpConnectorProxy : public IHttpConnectorHandler {
public:
    void SetCloseDone(TTCloseDone done, void* ud)
    {
        std::lock_guard<std::mutex> lk(mu_);
        done_ = done;
        ud_   = ud;
    }

    // 转发到 NSLog —— 空实现会把原生层的诊断全丢掉，而 force-physical /
    // URL 解析这类问题的现场只在库日志里。
    void OnLog(int level, LPCSTR msg) override
    {
        if (msg) NSLog(@"[ttsignal-http][%d] %s", level, msg);
    }

    void OnClosed() override
    {
        TTCloseDone done = nullptr;
        void*       ud   = nullptr;
        {
            std::lock_guard<std::mutex> lk(mu_);
            done = done_; ud = ud_;
            done_ = nullptr;      // 只触发一次
        }
        if (done) done(ud);
    }

private:
    std::mutex  mu_;
    TTCloseDone done_ = nullptr;
    void*       ud_   = nullptr;
};

///////////////////////////////////////////////////////////////////////////////
// 一次请求的 handler。契约：OnHttpResponse 与 OnHttpError **恰好触发一个**，
// 触发后自毁 —— 所以 completion 也恰好一次，Swift 侧的 continuation 才能安全
// resume（continuation 二次 resume 会直接 crash）。
///////////////////////////////////////////////////////////////////////////////

class TTHttpRequestCtx : public IHttpRequestHandler {
public:
    TTHttpCompletion completion = nullptr;
    void*            userdata   = nullptr;

    void OnHttpResponse(const HttpResponse& resp) override
    {
        // headers 摊成平行数组。c_str() 指向 resp 里的 std::string，而 resp 在
        // 本回调期间一直活着，completion 又是同步调用的 —— 指针有效。
        std::vector<const char*> names, values;
        names.reserve(resp.headers.size());
        values.reserve(resp.headers.size());
        for (const auto& kv : resp.headers) {
            names.push_back(kv.first.c_str());
            values.push_back(kv.second.c_str());
        }

        TTHttpResponse out;
        memset(&out, 0, sizeof(out));
        out.status       = (int32_t)resp.status;
        out.reason       = resp.reason.c_str();
        out.headerNames  = names.empty()  ? nullptr : names.data();
        out.headerValues = values.empty() ? nullptr : values.data();
        out.headerCount  = names.size();
        out.body         = resp.body.empty()
                         ? nullptr : (const uint8_t*)resp.body.data();
        out.bodyLen      = resp.body.size();
        out.peerIp       = resp.peerIp.c_str();
        out.boundIfIndex = resp.boundIfIndex;
        out.pinMethod    = resp.pinMethod.c_str();

        if (completion) completion(userdata, 0, nullptr, nullptr, &out);
        delete this;
    }

    void OnHttpError(BCRESULT result, const std::string& message) override
    {
        // ResultName 返回 std::string，得留个局部变量撑住 c_str()。
        const std::string name = node::ResultName(result);
        if (completion) {
            completion(userdata, (int32_t)result,
                       name.c_str(), message.c_str(), nullptr);
        }
        delete this;
    }
};

///////////////////////////////////////////////////////////////////////////////
// WSConnector::Create 要一个 IWSConnectorHandler。与 HTTP 侧的 proxy 同构。
///////////////////////////////////////////////////////////////////////////////

class TTWSConnectorProxy : public IWSConnectorHandler {
public:
    void SetCloseDone(TTCloseDone done, void* ud)
    {
        std::lock_guard<std::mutex> lk(mu_);
        done_ = done;
        ud_   = ud;
    }

    void OnLog(int level, LPCSTR msg) override
    {
        if (msg) NSLog(@"[ttsignal-ws][%d] %s", level, msg);
    }
    void OnException(BCException&) override {}

    void OnClosed() override
    {
        TTCloseDone done = nullptr;
        void*       ud   = nullptr;
        {
            std::lock_guard<std::mutex> lk(mu_);
            done = done_; ud = ud_;
            done_ = nullptr;
        }
        if (done) done(ud);
    }

private:
    std::mutex  mu_;
    TTCloseDone done_ = nullptr;
    void*       ud_   = nullptr;
};

///////////////////////////////////////////////////////////////////////////////
// 单条连接的 handler。**本文件最需要小心的地方**：什么时候把 userdata 还给
// 绑定层。
//
// WSConnector.h 顶部的契约：
//   1. Connect() 返回非 BC_R_SUCCESS 时不会有任何回调；
//   2. 返回 BC_R_SUCCESS 之后 OnConnectResult 恰好一次，且
//        result == SUCCESS  => 之后还会恰好一次 OnClosed；
//        result != SUCCESS  => **不再有 OnClosed**；
//   3. handler 必须活到最后一次回调返回。
//
// 于是"等 OnClosed 再释放"是错的 —— 握手失败的连接永远等不到，box 就永久泄漏。
// 把判定收在这里，对外只暴露一个 on_release，调用方不必理解这套契约。
///////////////////////////////////////////////////////////////////////////////

class TTWSConnAdapter : public IWSConnectionHandler {
public:
    WSConnPtr         conn;
    TTWSHandlerVTable vt;
    void*             userdata = nullptr;

    ///////////////////////////////////////////////////////////////////////
    // 销毁归属：**谁最后到，谁 delete**。
    //
    // 为什么不能在 tt_ws_connection_destroy 里直接 delete：WSConnection 把本
    // 对象记在 handler_ 上，而 ClearHandlerLocked() 是 private、只给
    // ~WSConnector 用 —— 绑定层没有任何办法主动解绑。业务一旦在握手途中放手
    // （Swift 那边 connection 出作用域即 deinit），随后到达的
    // OnChannelClosed -> _DeliverOnce -> _DeliverConnectResult 就会打在已释放
    // 的 handler 上。crash-2026.09.07.01 就是这条路径，栈精确落在
    // _DeliverConnectResult 调 h->OnConnectResult 那一行。
    //
    // 契约第 2 条最后一句写得很直白：**handler 必须活到最后一次回调返回**。
    // 所以生命周期由"最后一次回调"决定，不由业务的 destroy 决定。
    ///////////////////////////////////////////////////////////////////////

    // 回调侧：最后一次回调发完了。
    void ReleaseOnce()
    {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (released_) return;
            released_   = true;
            in_release_ = true;         // 占住对象，DetachFromOwner 不许删
        }
        // ⚠️ on_release 必须在**锁外**调：它释放的是绑定层的 box，Swift 那边
        // 的 deinit 可能顺着 TTSignalWSConnection 一路回调进
        // tt_ws_connection_destroy -> DetachFromOwner，锁内调就是自锁死。
        // 代价是这一段没有锁保护，所以用 in_release_ 而不是 detached_ 的快照
        // 来决定归属 —— 期间业务放手的话，doDelete 要重新算一次。
        if (vt.on_release) vt.on_release(userdata);

        bool doDelete = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            in_release_ = false;
            doDelete    = detached_;    // 业务已放手（含刚才那一段里放的）
        }
        if (doDelete) delete this;
    }

    // 业务侧：tt_ws_connection_destroy 调用。connectIssued 表示是否已经
    // 成功受理过 Connect() —— 没有的话按契约 1 不会有任何回调，可以就地销毁。
    void DetachFromOwner(bool connectIssued)
    {
        bool doDelete    = false;
        bool needRelease = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            detached_ = true;
            if (released_ && !in_release_) {
                doDelete = true;             // 回调已走完，安全
            } else if (in_release_) {
                // ReleaseOnce 正跑在这个对象上（on_release 在锁外）。此刻
                // delete 就是 use-after-free —— 它返回前还要回来读成员。
                // 让路，由 ReleaseOnce 收尾时看见 detached_ 再删。
            } else if (!connectIssued) {
                released_   = true;          // 契约 1：不会有回调
                needRelease = true;
                doDelete    = true;
            }
            // 否则：回调在途，交给 ReleaseOnce 收尾
        }
        if (needRelease && vt.on_release) vt.on_release(userdata);
        if (doDelete) delete this;
    }

    void OnConnectResult(BCRESULT result, const HttpHeaderMap&) override
    {
        // LastErrorMessage() 的值在派发回调**之前**就写好了，回调里一定读得到；
        // 任意线程可调。WSConnector.h:236-241 明确要求绑定层这样原样透传 ——
        // force-physical 的现场描述只在这一段里。
        std::string msg;
        if (conn) msg = conn->LastErrorMessage();
        if (vt.on_connect_result) {
            vt.on_connect_result(userdata, (int32_t)result, msg.c_str());
        }
        if (result != BC_R_SUCCESS) ReleaseOnce();   // 契约 2：不会再有 OnClosed
    }

    void OnRecvText(LPCSTR text) override
    {
        if (vt.on_text && text) vt.on_text(userdata, text, strlen(text));
    }

    void OnRecvData(LPCVOID data, size_t size) override
    {
        if (vt.on_data) vt.on_data(userdata, (const uint8_t*)data, size);
    }

    void OnClosed(LPCSTR reason) override
    {
        if (vt.on_closed) vt.on_closed(userdata, reason);
        ReleaseOnce();
    }

    void OnException(BCException& e) override
    {
        // BCException 的取值接口是 GetMsg()，不是标准库的 what()。
        const std::string msg = e.GetMsg().c_str();
        if (vt.on_exception) vt.on_exception(userdata, msg.c_str());
    }

private:
    std::mutex mu_;
    bool       released_   = false; // 最后一次回调已发
    bool       in_release_ = false; // ReleaseOnce 正在跑（on_release 在锁外）
    bool       detached_ = false;   // 业务已 destroy
};

} // namespace ios_http_bridge

using namespace ios_http_bridge;

extern "C" {

///////////////////////////////////////////////////////////////////////////////
// 同步失败的现场描述。
//
// 同步路径上没有 completion / 回调可以带 message，而 URL 解析、请求组装、
// force-physical 拿不到物理网卡这几种恰恰都是**同步**失败 —— 不接出来的话，
// 绑定层能交给业务的就只剩一个错误码。
//
// 线程局部，跟 tt_result_name 同一套路：调用方在同一条线程上、紧接着失败的那次
// 调用去取，下一次同步调用才覆盖。
///////////////////////////////////////////////////////////////////////////////

static thread_local std::string t_sync_error;

static void SetSyncError(const std::string& msg) { t_sync_error = msg; }

const char* tt_last_sync_error(void)
{
    return t_sync_error.c_str();
}

const char* tt_result_name(int32_t result)
{
    // 线程局部：返回值是 const char*，得有个地方存住 std::string 的存储。
    // 每线程一份，下一次调用才覆盖 —— 调用方在用完之前不会再调。
    static thread_local std::string s_name;
    s_name = node::ResultName((BCRESULT)result);
    return s_name.c_str();
}

///////////////////////////////////////////////////////////////////////////////
// HTTP —— 一次性请求
//
// 与上面 SMP/QUIC 那套完全独立：另一条 TCP 栈，另一个 connector。
///////////////////////////////////////////////////////////////////////////////

struct TTHttpConnector {
    HttpConnector*            connector = nullptr;
    TTHttpConnectorProxy*     proxy     = nullptr;
};

TTHttpConnectorRef tt_http_connector_create(const TTHttpConfig* config)
{
    if (!config) return nullptr;

    BCFObject cfg;
    FillNetConfig(&cfg, config->vpnPolicy, config->caCerts, config->spkiPin,
                  config->insecureSkipVerify, config->dnsServers,
                  config->dnsTimeoutMs, config->drainTimeoutMs,
                  config->logLevel);
    if (config->maxResponseBytes) {
        cfg.PutInt("maxResponseBytes", (int)config->maxResponseBytes);
    }

    // Runtime 必须先起来：TcpChannel::Open 里 BCEventQueue::Create 要拿
    // Runtime::RandomTimerMgr() / RandomTaskMgr()，没初始化就是 BC_R_INVALIDARG，
    // 而且那条路径**不打日志**，现场只有一个 63。
    //
    // ⚠️ 这个坑很隐蔽：force-physical 的硬校验刻意排在 BCEventQueue::Create
    // 之前（TcpChannel.cpp 注释说明是为了能在不初始化 Runtime 的前提下单测），
    // 所以 force-physical 会正常报 64、看起来是好的，只有 os / prefer 才
    // 暴露出缺 Runtime。
    //
    // 已初始化时 Runtime::Initialize 直接返回成功，不会覆盖已有线程配置 ——
    // 与 NAPI 侧（JsHttpConnectorWrap.cpp:199-201）同一处理。
    if (Runtime::Initialize(&cfg) != BC_R_SUCCESS) return nullptr;

    TTHttpConnector* self = new TTHttpConnector();
    self->proxy     = new TTHttpConnectorProxy();
    self->connector = new HttpConnector();
    if (self->connector->Create(&cfg, self->proxy) != BC_R_SUCCESS) {
        delete self->connector;
        delete self->proxy;
        delete self;
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
    if (req->headerNames && req->headerValues) {
        for (size_t i = 0; i < req->headerCount; ++i) {
            if (req->headerNames[i] && req->headerValues[i]) {
                r.headers[req->headerNames[i]] = req->headerValues[i];
            }
        }
    }
    if (req->body && req->bodyLen) {
        r.body.assign((const char*)req->body, req->bodyLen);
    }
    if (req->timeoutMs)                     r.timeoutMs  = req->timeoutMs;
    if (req->resolvedIp && req->resolvedIp[0]) r.resolvedIp = req->resolvedIp;

    TTHttpRequestCtx* ctx = new TTHttpRequestCtx();
    ctx->completion = completion;
    ctx->userdata   = userdata;

    std::string syncErr;
    BCRESULT rc = cref->connector->Request(r, ctx, &syncErr);
    if (rc != BC_R_SUCCESS) {
        // 没受理 => 两个回调都不会来，ctx 自己回收。userdata 由调用方负责，
        // 契约写在 ios_bridge.h 上。
        SetSyncError(syncErr);
        delete ctx;
        return (int32_t)rc;
    }
    SetSyncError(std::string());
    return 0;
}

void tt_http_connector_close(TTHttpConnectorRef cref, TTCloseDone done, void* ud)
{
    if (!cref) { if (done) done(ud); return; }
    cref->proxy->SetCloseDone(done, ud);
    cref->connector->Close();
}

void tt_http_connector_destroy(TTHttpConnectorRef cref)
{
    if (!cref) return;
    delete cref->connector;   // 析构会等在途回调收尾（最多 drainTimeoutMs）
    delete cref->proxy;
    delete cref;
}

///////////////////////////////////////////////////////////////////////////////
// WebSocket —— 长连接
///////////////////////////////////////////////////////////////////////////////

struct TTWSConnector {
    WS::WSConnector*    connector = nullptr;
    TTWSConnectorProxy* proxy     = nullptr;
};

struct TTWSConnection {
    TTWSConnAdapter* adapter        = nullptr;
    // Connect() 是否成功受理过。决定 destroy 时能否就地销毁 adapter
    // （契约 1：Connect 返回非成功时不会有任何回调）。
    bool             connect_issued = false;
};

TTWSConnectorRef tt_ws_connector_create(const TTWSConfig* config)
{
    if (!config) return nullptr;

    BCFObject cfg;
    FillNetConfig(&cfg, config->vpnPolicy, config->caCerts, config->spkiPin,
                  config->insecureSkipVerify, config->dnsServers,
                  config->dnsTimeoutMs, config->drainTimeoutMs,
                  config->logLevel);
    // WS 专有。0 / 空串一律不写键，让原生层用自己的默认值。
    if (config->connectTimeoutMs)
        cfg.PutInt("connectTimeoutMs", (int)config->connectTimeoutMs);
    if (config->pingIntervalMs)
        cfg.PutInt("pingIntervalMs",   (int)config->pingIntervalMs);
    if (config->idleTimeoutMs)
        cfg.PutInt("idleTimeoutMs",    (int)config->idleTimeoutMs);
    if (config->maxFrameBytes)
        cfg.PutInt("maxFrameBytes",    (int)config->maxFrameBytes);
    if (config->clientCertFile && config->clientCertFile[0])
        cfg.PutString("clientCertFile", config->clientCertFile);
    if (config->clientKeyFile && config->clientKeyFile[0])
        cfg.PutString("clientKeyFile", config->clientKeyFile);
    if (config->clientKeyPassword && config->clientKeyPassword[0])
        cfg.PutString("clientKeyPassword", config->clientKeyPassword);

    // 同 tt_http_connector_create：TcpChannel 要 Runtime 的 timer/task mgr。
    if (Runtime::Initialize(&cfg) != BC_R_SUCCESS) return nullptr;

    TTWSConnector* self = new TTWSConnector();
    self->proxy     = new TTWSConnectorProxy();
    self->connector = new WS::WSConnector();
    if (self->connector->Create(&cfg, self->proxy) != BC_R_SUCCESS) {
        delete self->connector;
        delete self->proxy;
        delete self;
        return nullptr;
    }
    return self;
}

TTWSConnectionRef tt_ws_connector_create_connection(TTWSConnectorRef cref,
        const TTWSHandlerVTable* vtable, void* userdata)
{
    if (!cref || !vtable) return nullptr;

    TTWSConnAdapter* ad = new TTWSConnAdapter();
    ad->vt       = *vtable;      // 拷贝，调用方不必保留
    ad->userdata = userdata;
    // 连接级配置传 NULL：沿用连接器的默认配置。逐键覆盖不在本次范围
    // （Node 侧支持，但 iOS 暂无消费方）。
    ad->conn = cref->connector->CreateConnection(nullptr, ad);
    if (!ad->conn) {
        // 一个回调都不会有，userdata 原样还给调用方并就地销毁。
        ad->DetachFromOwner(/*connectIssued=*/false);
        return nullptr;
    }

    TTWSConnection* self = new TTWSConnection();
    self->adapter = ad;
    return self;
}

void tt_ws_connector_close(TTWSConnectorRef cref, TTCloseDone done, void* ud)
{
    if (!cref) { if (done) done(ud); return; }
    cref->proxy->SetCloseDone(done, ud);
    cref->connector->Close();
}

void tt_ws_connector_destroy(TTWSConnectorRef cref)
{
    if (!cref) return;
    delete cref->connector;
    delete cref->proxy;
    delete cref;
}

int32_t tt_ws_connection_connect(TTWSConnectionRef c, const char* url,
                                 uint32_t timeoutMs)
{
    if (!c || !c->adapter || !c->adapter->conn || !url) return BC_R_INVALIDARG;
    BCRESULT rc = c->adapter->conn->Connect(url, timeoutMs);
    if (rc == BC_R_SUCCESS) {
        c->connect_issued = true;
        SetSyncError(std::string());
    } else {
        // 同步失败时 WSConnection::Connect 已经把现场描述写进 LastErrorMessage()
        SetSyncError(c->adapter->conn->LastErrorMessage());
        // 契约 1：非成功返回时不会有任何回调 —— 就地归还 userdata，否则永久泄漏。
        c->adapter->ReleaseOnce();
    }
    return (int32_t)rc;
}

int32_t tt_ws_connection_set_request_header(TTWSConnectionRef c,
                                            const char* name, const char* value)
{
    if (!c || !c->adapter || !c->adapter->conn || !name || !value)
        return BC_R_INVALIDARG;
    return (int32_t)c->adapter->conn->SetRequestHeader(name, value);
}

int32_t tt_ws_connection_send_text(TTWSConnectionRef c, const char* text,
                                   size_t len)
{
    if (!c || !c->adapter || !c->adapter->conn || !text) return BC_R_INVALIDARG;
    return (int32_t)c->adapter->conn->SendText(std::string(text, len));
}

int32_t tt_ws_connection_send_data(TTWSConnectionRef c, const uint8_t* data,
                                   size_t len)
{
    if (!c || !c->adapter || !c->adapter->conn || !data) return BC_R_INVALIDARG;
    return (int32_t)c->adapter->conn->SendData(data, len);
}

int32_t tt_ws_connection_send_ping(TTWSConnectionRef c)
{
    if (!c || !c->adapter || !c->adapter->conn) return BC_R_INVALIDARG;
    return (int32_t)c->adapter->conn->SendPing();
}

uint32_t tt_ws_connection_bound_if(TTWSConnectionRef c)
{
    if (!c || !c->adapter || !c->adapter->conn) return 0;
    return c->adapter->conn->BoundIfIndex();
}

void tt_ws_connection_close(TTWSConnectionRef c)
{
    if (!c || !c->adapter || !c->adapter->conn) return;
    c->adapter->conn->Close();
}

void tt_ws_connection_destroy(TTWSConnectionRef c)
{
    if (!c) return;
    TTWSConnAdapter* ad       = c->adapter;
    const bool       issued   = c->connect_issued;
    delete c;                       // 包装层可以立即销毁
    if (!ad) return;

    // 握手/连接可能还在进行：先促成收尾，否则最后一次回调永远不来，adapter
    // 就永远等不到销毁时机。Close() 幂等，也可以在从没 connect 的连接上调。
    if (issued && ad->conn) ad->conn->Close();

    // ⚠️ 绝不在这里 delete ad —— 回调可能正在路上。交给"谁最后到谁销毁"。
    ad->DetachFromOwner(issued);
}

} // extern "C"

///////////////////////////////////////////////////////////////////////////////
// End of file : ios_http_bridge.mm
///////////////////////////////////////////////////////////////////////////////
