///////////////////////////////////////////////////////////////////////////////
// file : ios_bridge.mm
// author : anto
//
// extern "C" Swift-facing surface implemented on top of SMPConnector /
// SMPConnection. This file is the iOS equivalent of
// src/cpp/jni/JNI_SMPConnectorWrap.cpp + JNI_SMPacketWrap.cpp.
///////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "ios_bridge.h"
#include "ios_logger.h"
#include "BC/BCJson.h"
#include "BC/BCFCodec.h"
#include "Utils.h"
#include "SMPConnector.h"
#include "Runtime.h"
#include "SMPacket.h"
#include "Interface.h"
#include "http-parser/http_parser.h"

#include <string>
#include <memory>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <chrono>

#define MY_TLS_GROUPS "X25519:P-384:P-521"

// These three opaque structs MUST live in the global namespace — they're
// forward-declared in ios_bridge.h via typedef struct TTConnector* etc., and
// the C-linkage TTConnectorRef / TTConnectionRef / TTPacketRef typedefs in
// the header all resolve to ::TTConnector* etc. Putting them inside a
// namespace makes the typedef forward-declaration and the definition refer
// to *different* types.
struct TTConnector;
struct TTConnection;
struct TTPacket;

namespace ios_bridge {

///////////////////////////////////////////////////////////////////////////////
// helpers
///////////////////////////////////////////////////////////////////////////////

static const char* SafeStr(const char* s) { return s ? s : ""; }
static bool        BoolField(int32_t v)    { return v != 0; }

// Convert a TTConfig (Swift-populated POD) into the BCFObject that
// SMPConnector / SMPConnection expect. Mirrors JNI_SMPConfig.cpp.
static BCFObject* ConvertConfig(const TTConfig* cfg)
{
    if (!cfg) return nullptr;
    BCFObject* p = new BCFObject();
    if (!p) return nullptr;

    p->PutInt("taskThreads",                  cfg->taskThreads > 0 ? cfg->taskThreads : 16);
    p->PutInt("timerThreads",                 cfg->timerThreads > 0 ? cfg->timerThreads : 4);
    p->PutInt("idle_time_out",                cfg->idleTimeOut > 0 ? cfg->idleTimeOut : 20000);
    p->PutString("alpn",                      SafeStr(cfg->alpn));
    p->PutString("hostname",                  SafeStr(cfg->hostname));
    p->PutInt("port",                         cfg->port);
    p->PutInt("backlog",                      cfg->backlog);
    p->PutBool("reuse_port",                  BoolField(cfg->reusePort));
    p->PutInt("max_conn",                     cfg->maxConnections);
    p->PutInt("c_cong_ctl",                   cfg->congestCtrl);
    p->PutBool("ping_on",                     BoolField(cfg->pingOn));
    p->PutInt("ping_interval",                cfg->pingInterval);
    p->PutInt("active_connection_id_limit",   cfg->activeConnectionIdLimit);
    p->PutInt("device_type",                  cfg->deviceType);
    p->PutString("cid_tag",                   SafeStr(cfg->cidTag));
    p->PutBool("ssl",                         BoolField(cfg->ssl));
    p->PutString("private_key_file",          SafeStr(cfg->privateKeyFile));
    p->PutString("certificate_file",          SafeStr(cfg->certificateFile));
    if (cfg->logFile && cfg->logFile[0]) {
        p->PutString("log_file",              cfg->logFile);
    }
    p->PutInt("log_level",                    cfg->logLevel);
    p->PutInt("num_of_senders",               cfg->numOfSenders > 0 ? cfg->numOfSenders : 1);
    if (cfg->serverHost && cfg->serverHost[0]) {
        p->PutString("server_host",           cfg->serverHost);
    }
    if (cfg->caCertPem && cfg->caCertPem[0]) {
        p->PutString("ca_cert_pem",           cfg->caCertPem);
    }
    // SMPConnector reads this with the exact `disableAutoRestart` key
    // (see SMPConnector.cpp ~line 1855 + the !config_.disableAutoRestart
    // branch around tt_netmon_start). Always emit it so server-side
    // deployments can opt out without depending on the C-struct default
    // happening to be 0.
    p->PutBool("disableAutoRestart",          BoolField(cfg->disableAutoRestart));
    return p;
}

///////////////////////////////////////////////////////////////////////////////
// IOSHandlerProxy — forwards IConnectionHandler callbacks to a C vtable
///////////////////////////////////////////////////////////////////////////////

class IOSHandlerProxy : public IConnectionHandler {
public:
    IOSHandlerProxy(const TTHandlerVTable* vtable, void* userdata)
        : vtable_(*vtable), userdata_(userdata) {}
    ~IOSHandlerProxy() override = default;

    void OnHandshakeFinished() override {}

    void OnExecDone(IRPCStub* pStub) override {
        if (!pStub) return;
        if (kConnect.Equal(pStub->m_szCmd)) {
            const char* msg = nullptr;
            std::string holder;
            if (pStub->m_lParams[0] && pStub->m_lParams[1]) {
                holder.assign((const char*)pStub->m_lParams[0],
                              (size_t)pStub->m_lParams[1]);
                msg = holder.c_str();
            }
            if (vtable_.on_connect_result) {
                vtable_.on_connect_result(userdata_, (int32_t)pStub->m_result, msg);
            }
        }
        // Stub lifecycle is owned by SMPConnection — it cleans up via its
        // local stub manager when the call resolves. We don't free here.
    }

    void OnStreamCreated(uint32_t nStreamId) override {
        if (vtable_.on_stream_created) vtable_.on_stream_created(userdata_, (int32_t)nStreamId);
    }

    void OnStreamClosed(uint32_t nStreamId) override {
        if (vtable_.on_stream_closed) vtable_.on_stream_closed(userdata_, (int32_t)nStreamId);
    }

    void OnStreamDataAcked(uint32_t nStreamId,
                           xqc_usec_t ack_delay_time,
                           size_t acked_bytes,
                           size_t inflight_bytes) override
    {
        if (vtable_.on_stream_data_acked) {
            vtable_.on_stream_data_acked(userdata_,
                                         (int32_t)nStreamId,
                                         (int64_t)ack_delay_time,
                                         (int32_t)acked_bytes,
                                         (int32_t)inflight_bytes);
        }
    }

    void OnStreamDataSent(uint32_t nStreamId,
                          uint32_t nTransId,
                          size_t size) override
    {
        if (vtable_.on_stream_data_sent) {
            vtable_.on_stream_data_sent(userdata_,
                                        (int32_t)nStreamId,
                                        (int32_t)nTransId,
                                        (int32_t)size);
        }
    }

    void OnRecvCmd(const SMPHeader& refHeader,
                   const char* lpszCmd, size_t msg_size) override
    {
        if (vtable_.on_recv_cmd) {
            vtable_.on_recv_cmd(userdata_,
                                (int64_t)refHeader.m_nTimestamp,
                                (int32_t)refHeader.m_nTransId,
                                (int32_t)refHeader.m_nStreamId,
                                (const uint8_t*)lpszCmd,
                                msg_size);
        }
    }

    void OnRecvData(const SMPHeader& refHeader,
                    LPCVOID lpszMsg, size_t msg_size) override
    {
        if (vtable_.on_recv_data) {
            vtable_.on_recv_data(userdata_,
                                 (int64_t)refHeader.m_nTimestamp,
                                 (int32_t)refHeader.m_nTransId,
                                 (int32_t)refHeader.m_nStreamId,
                                 (const uint8_t*)lpszMsg,
                                 msg_size);
        }
    }

    void OnRestart(BCRESULT result, const char* lpszAddr) override {
        if (vtable_.on_restart) {
            vtable_.on_restart(userdata_, (int32_t)result, lpszAddr ? lpszAddr : "");
        }
    }

    void OnClosed(LPCSTR strReason) override {
        if (vtable_.on_closed) {
            vtable_.on_closed(userdata_, strReason ? strReason : "");
        }
    }

    void OnException(BCException& except) override {
        if (vtable_.on_exception) {
            vtable_.on_exception(userdata_, except.GetMsg().c_str());
        }
    }

private:
    TTHandlerVTable vtable_{};
    void*           userdata_;
};

///////////////////////////////////////////////////////////////////////////////
// IOSConnectorHandlerProxy — forwards SMPConnector log lines to the
// global tt_logger callback (set via tt_logger_set_callback).
///////////////////////////////////////////////////////////////////////////////

class IOSConnectorHandlerProxy : public IConnectorHandler {
public:
    IOSConnectorHandlerProxy() = default;
    ~IOSConnectorHandlerProxy() override = default;

    void OnExecDone(IRPCStub*) override {}
    void OnException(BCException&) override {}
    void OnLog(int level, LPCSTR lpszMsg) override {
        ios_logger_dispatch(level, lpszMsg);
    }

    /// SMPConnector::_CloseCheck() PostTask's into here once the engine
    /// has been torn down (xqc_engine_destroy + log appender removed).
    /// We just flip the flag and wake anybody parked in WaitForClosed —
    /// safe to fire even when nobody is waiting (normal app shutdown
    /// path that uses tt_connector_close + tt_connector_destroy without
    /// the sync helper).
    void OnClosed() override {
        std::lock_guard<std::mutex> g(close_mu_);
        closed_ = true;
        close_cv_.notify_all();
    }

    /// Block the calling thread until OnClosed has fired or `timeout_ms`
    /// elapses. Returns `true` iff OnClosed actually fired.
    ///
    /// `timeout_ms <= 0` waits forever — only do that if you're certain
    /// the connector will eventually close, otherwise you leak the
    /// thread.
    ///
    /// MUST NOT be called from a Runtime worker thread (i.e. from
    /// inside any TTHandlerVTable callback). The OnClosed notification
    /// itself is dispatched onto the Runtime via PostTask, so a
    /// worker-thread waiter would deadlock the same pool that has to
    /// deliver the wake-up.
    bool WaitForClosed(int32_t timeout_ms) {
        std::unique_lock<std::mutex> g(close_mu_);
        if (closed_) return true;
        if (timeout_ms <= 0) {
            close_cv_.wait(g, [this] { return closed_; });
            return true;
        }
        return close_cv_.wait_for(g,
                                  std::chrono::milliseconds(timeout_ms),
                                  [this] { return closed_; });
    }

private:
    std::mutex              close_mu_;
    std::condition_variable close_cv_;
    bool                    closed_ = false;
};

} // namespace ios_bridge

///////////////////////////////////////////////////////////////////////////////
// TTConnector — owns SMPConnector + connector handler.
//
// Path-change monitoring is now driven entirely by SMPConnector itself
// (see TT_HAS_PATH_MONITOR + tt_netmon_start in SMPConnector.cpp). The
// bridge no longer maintains its own NWPathMonitor or per-connector
// connection list — that responsibility moved into the cross-platform
// SMPConnector layer so macOS / Linux / Windows / iOS share one
// implementation.
///////////////////////////////////////////////////////////////////////////////

struct TTConnector {
    SMPConnector*                            connector  = nullptr;
    ios_bridge::IOSConnectorHandlerProxy*    handler    = nullptr;
};

///////////////////////////////////////////////////////////////////////////////
// TTConnection — owns one SMPConnection + its handler proxy
///////////////////////////////////////////////////////////////////////////////

struct TTConnection {
    SMPConnection*               connection = nullptr;
    ios_bridge::IOSHandlerProxy* handler    = nullptr;
    TTConnector*                 parent     = nullptr;  // weak — for symmetry
};

///////////////////////////////////////////////////////////////////////////////
// TTPacket — owns one SMPacket
///////////////////////////////////////////////////////////////////////////////

struct TTPacket {
    SMPacketPtr packet;
};

///////////////////////////////////////////////////////////////////////////////
// extern "C" API
///////////////////////////////////////////////////////////////////////////////

using namespace ios_bridge;

extern "C" {

TTConnectorRef tt_connector_create(const TTConfig* config)
{
    std::unique_ptr<BCFObject> bcfg(ConvertConfig(config));
    if (!bcfg) return nullptr;

    TTConnector* self = new TTConnector();
    self->handler   = new IOSConnectorHandlerProxy();
    self->connector = new SMPConnector();
    if (!self->connector) {
        delete self->handler;
        delete self;
        return nullptr;
    }

    Runtime::Initialize(bcfg.get());

    BCFObject sockConfig;
    sockConfig.PutBool("ipv6", 0);
    if (config->serverHost && config->serverHost[0]) {
        sockConfig.PutString("server_host", config->serverHost);
    } else if (config->hostname && config->hostname[0]) {
        sockConfig.PutString("server_host", config->hostname);
    } else {
        sockConfig.PutString("server_host", "example.com");
    }
    sockConfig.PutString("tls_ciphers", XQC_TLS_CIPHERS);
    sockConfig.PutString("tls_groups", MY_TLS_GROUPS);
    sockConfig.PutInt("pacing_on", 0);
    *bcfg += sockConfig;

    BCRESULT r = self->connector->Create(bcfg.get(), self->handler);
    if (r != BC_R_SUCCESS) {
        delete self->connector;
        delete self->handler;
        delete self;
        return nullptr;
    }

    // SMPConnector::Create() has already started the AppleNetworkMonitor (or
    // skipped it if config.disableAutoRestart is true). Application code on
    // iOS never has to call connection.restart() manually — path changes flow
    // through SMPConnector::OnPathChange directly into SMPConnection::Restart.
    return self;
}

TTConnectionRef tt_connector_create_connection(TTConnectorRef cref,
                                               const TTConfig* config,
                                               const TTHandlerVTable* vtable,
                                               void* userdata)
{
    if (!cref || !cref->connector || !vtable) return nullptr;

    std::unique_ptr<BCFObject> bcfg(ConvertConfig(config));
    if (!bcfg) return nullptr;

    TTConnection* tc = new TTConnection();
    tc->parent  = cref;
    tc->handler = new IOSHandlerProxy(vtable, userdata);

    SMPConnection* conn = cref->connector->CreateConnection(tc->handler, bcfg.get());
    if (!conn) {
        delete tc->handler;
        delete tc;
        return nullptr;
    }
    tc->connection = conn;
    return tc;
}

size_t tt_connector_get_stats(TTConnectorRef cref, char* outBuf, size_t bufSize)
{
    if (!cref || !cref->connector || !outBuf || bufSize < 32) return 0;
    ConnStatS stats;
    cref->connector->GetStats(stats);

    Json::Value v(Json::objectValue);
    auto m = stats.ToMap();
    // BC's bundled JsonCpp predates UInt64 — render the size_t values as
    // strings to guarantee no 32-bit overflow on long-lived sessions.
    for (auto& kv : m) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%llu", (unsigned long long)kv.second);
        v[kv.first] = std::string(buf);
    }
    Json::FastWriter w;
    std::string s = w.write(v);
    if (!s.empty() && s.back() == '\n') s.pop_back();

    if (s.size() + 1 > bufSize) return 0;
    memcpy(outBuf, s.c_str(), s.size());
    outBuf[s.size()] = '\0';
    return s.size();
}

void tt_connector_close(TTConnectorRef cref)
{
    if (!cref) return;
    if (cref->connector) {
        // SMPConnector dtor (via tt_connector_destroy below) tears down the
        // path monitor automatically, so there's no separate netmon stop here.
        cref->connector->Close();
    }
}

int32_t tt_connector_close_sync(TTConnectorRef cref, int32_t timeoutMs)
{
    if (!cref || !cref->connector || !cref->handler) return BC_R_INVALIDARG;
    // Kick the (asynchronous) close, then park on the handler's cv until
    // _CloseCheck fires the OnClosed callback. After we return, the
    // SMPConnector has finished xqc_engine_destroy and dropped its log
    // appender, so it's safe for the caller to immediately call
    // tt_connector_destroy without racing the engine teardown threads.
    cref->connector->Close();
    return cref->handler->WaitForClosed(timeoutMs)
               ? BC_R_SUCCESS
               : BC_R_TIMEDOUT;
}

void tt_connector_destroy(TTConnectorRef cref)
{
    if (!cref) return;
    // Connector::Close should already have fired OnClosed and the engine
    // should be torn down. Delete the SMPConnector here, mirroring
    // SMPConnectorWrap's destructor.
    if (cref->connector) {
        delete cref->connector;
        cref->connector = nullptr;
    }
    if (cref->handler) {
        delete cref->handler;
        cref->handler = nullptr;
    }
    delete cref;
}

///////////////////////////////////////////////////////////////////////////////
// Connection
///////////////////////////////////////////////////////////////////////////////

int32_t tt_connection_connect(TTConnectionRef tc,
                              const char* url,
                              const char* propsJson,
                              int32_t timeoutMs)
{
    if (!tc || !tc->connection || !url) return BC_R_INVALIDARG;

    std::string url_str = EncodeURI(std::string(url));

    struct http_parser_url u;
    http_parser_url_init(&u);
    if (http_parser_parse_url(url_str.c_str(), url_str.length(), 0, &u) != 0) {
        return BC_R_INVALIDARG;
    }

    auto field = [&](http_parser_url_fields f) -> std::string {
        if (u.field_set & (1 << f)) {
            return url_str.substr(u.field_data[f].off, u.field_data[f].len);
        }
        return "";
    };
    std::string scheme = field(UF_SCHEMA);
    std::string host   = field(UF_HOST);
    uint16_t    port   = (u.field_set & (1 << UF_PORT))
                            ? u.port
                            : (scheme == "https" ? 443 : 80);
    std::string path   = field(UF_PATH);
    std::string query  = field(UF_QUERY);

    // Validate the props JSON early so callers don't get silent server
    // errors (matches the JNI behaviour).
    if (propsJson && propsJson[0]) {
        Json::Reader rdr;
        Json::Value  v;
        if (!rdr.parse(std::string(propsJson), v) || !v.isObject()) {
            return BC_R_INVALIDARG;
        }
    }

    // Allocate a stub for this Connect call. SMPConnection::Connect takes
    // ownership of the stub (it's freed via the stub manager once the
    // result is delivered through OnExecDone).
    IRPCStub* stub = new IRPCStub(0);
    if (!stub) return BC_R_NOMEMORY;
    stub->SetCmdName(kConnect.Ptr);

    KBPool& pool = stub->m_sPool;
    stub->m_lParams[0] = (uint64_t)pool.Strdup(scheme.c_str());
    stub->m_lParams[1] = (uint64_t)pool.Strdup(host.c_str());
    stub->m_lParams[2] = port;
    stub->m_lParams[3] = (uint64_t)pool.Strdup(path.c_str());
    stub->m_lParams[4] = (uint64_t)pool.Strdup(query.c_str());
    stub->m_lParams[5] = (uint64_t)(propsJson && propsJson[0]
                                      ? pool.Strdup(propsJson)
                                      : nullptr);
    stub->m_lParams[6] = (uint64_t)(timeoutMs > 0 ? timeoutMs : 0);

    BCRESULT r = tc->connection->Connect(stub);
    if (r != BC_R_SUCCESS) {
        delete stub;
    }
    return (int32_t)r;
}

int32_t tt_connection_send_packet(TTConnectionRef tc, TTPacketRef pkt)
{
    if (!tc || !tc->connection || !pkt || !pkt->packet) return BC_R_INVALIDARG;
    return (int32_t)tc->connection->SendPacket(pkt->packet);
}

void tt_connection_restart(TTConnectionRef tc, int64_t networkHandle)
{
    if (!tc || !tc->connection) return;
    tc->connection->Restart(networkHandle);
}

void tt_connection_close(TTConnectionRef tc)
{
    if (!tc || !tc->connection) return;
    tc->connection->Close();
}

void tt_connection_close_stream(TTConnectionRef tc, int32_t streamId)
{
    if (!tc || !tc->connection) return;
    tc->connection->CloseStream((uint32_t)streamId);
}

void tt_connection_destroy(TTConnectionRef tc)
{
    if (!tc) return;

    // SMPConnection lifetime is tracked by the engine — Close() above will
    // eventually fire OnClosed which sets connection_handle = 0 in Java
    // land. Here we delete the C++ SMPConnection and the proxy in destroy
    // (the matching Java path is SMPConnectionWrap::_Cleanup -> delete
    // m_pConn, plus SMPConnectionWrap::_Destroy -> delete this).
    if (tc->connection) {
        delete tc->connection;
        tc->connection = nullptr;
    }
    if (tc->handler) {
        delete tc->handler;
        tc->handler = nullptr;
    }
    delete tc;
}

///////////////////////////////////////////////////////////////////////////////
// Packet
///////////////////////////////////////////////////////////////////////////////

TTPacketRef tt_packet_create(uint8_t type,
                             int64_t timestamp,
                             int32_t transId,
                             int32_t streamId,
                             const uint8_t* data,
                             size_t len)
{
    TTPacket* p = new TTPacket();
    p->packet = std::make_shared<SMPacket>();
    p->packet->type      = type;
    p->packet->timestamp = (uint64_t)(timestamp != 0 ? timestamp : (int64_t)bc_time_now());
    p->packet->trans_id  = (uint32_t)transId;
    p->packet->stream_id = (uint32_t)streamId;

    if (data && len > 0) {
        auto buf = std::make_shared<BCBuffer>();
        buf->Write(data, len);
        p->packet->Create(buf);
    }
    p->packet->PackPacket();
    return p;
}

void tt_packet_destroy(TTPacketRef p)
{
    if (!p) return;
    p->packet.reset();
    delete p;
}

const char* tt_get_sdk_version(void)
{
    return GetSDKVersion();
}

} // extern "C"
