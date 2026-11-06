
















#include "StdAfx.h"
#include "ios_bridge.h"
#include "BC/BCFCodec.h"
#include "Utils.h"          
#include "VpnPolicy.h"      
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






class TTHttpConnectorProxy : public IHttpConnectorHandler {
public:
    void SetCloseDone(TTCloseDone done, void* ud)
    {
        std::lock_guard<std::mutex> lk(mu_);
        done_ = done;
        ud_   = ud;
    }

    
    
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
            done_ = nullptr;      
        }
        if (done) done(ud);
    }

private:
    std::mutex  mu_;
    TTCloseDone done_ = nullptr;
    void*       ud_   = nullptr;
};







class TTHttpRequestCtx : public IHttpRequestHandler {
public:
    TTHttpCompletion completion = nullptr;
    void*            userdata   = nullptr;

    void OnHttpResponse(const HttpResponse& resp) override
    {
        
        
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
        
        const std::string name = node::ResultName(result);
        if (completion) {
            completion(userdata, (int32_t)result,
                       name.c_str(), message.c_str(), nullptr);
        }
        delete this;
    }
};





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
















class TTWSConnAdapter : public IWSConnectionHandler {
public:
    WSConnPtr         conn;
    TTWSHandlerVTable vt;
    void*             userdata = nullptr;

    
    
    
    
    
    
    
    
    
    
    
    
    
    

    
    void ReleaseOnce()
    {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (released_) return;
            released_   = true;
            in_release_ = true;         
        }
        
        
        
        
        
        if (vt.on_release) vt.on_release(userdata);

        bool doDelete = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            in_release_ = false;
            doDelete    = detached_;    
        }
        if (doDelete) delete this;
    }

    
    
    void DetachFromOwner(bool connectIssued)
    {
        bool doDelete    = false;
        bool needRelease = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            detached_ = true;
            if (released_ && !in_release_) {
                doDelete = true;             
            } else if (in_release_) {
                
                
                
            } else if (!connectIssued) {
                released_   = true;          
                needRelease = true;
                doDelete    = true;
            }
            
        }
        if (needRelease && vt.on_release) vt.on_release(userdata);
        if (doDelete) delete this;
    }

    void OnConnectResult(BCRESULT result, const HttpHeaderMap&) override
    {
        
        
        
        std::string msg;
        if (conn) msg = conn->LastErrorMessage();
        if (vt.on_connect_result) {
            vt.on_connect_result(userdata, (int32_t)result, msg.c_str());
        }
        if (result != BC_R_SUCCESS) ReleaseOnce();   
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
        
        const std::string msg = e.GetMsg().c_str();
        if (vt.on_exception) vt.on_exception(userdata, msg.c_str());
    }

private:
    std::mutex mu_;
    bool       released_   = false; 
    bool       in_release_ = false; 
    bool       detached_ = false;   
};

} 

using namespace ios_http_bridge;

extern "C" {












static thread_local std::string t_sync_error;

static void SetSyncError(const std::string& msg) { t_sync_error = msg; }

const char* tt_last_sync_error(void)
{
    return t_sync_error.c_str();
}

const char* tt_result_name(int32_t result)
{
    
    
    static thread_local std::string s_name;
    s_name = node::ResultName((BCRESULT)result);
    return s_name.c_str();
}







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
    delete cref->connector;   
    delete cref->proxy;
    delete cref;
}





struct TTWSConnector {
    WS::WSConnector*    connector = nullptr;
    TTWSConnectorProxy* proxy     = nullptr;
};

struct TTWSConnection {
    TTWSConnAdapter* adapter        = nullptr;
    
    
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
    ad->vt       = *vtable;      
    ad->userdata = userdata;
    
    
    ad->conn = cref->connector->CreateConnection(nullptr, ad);
    if (!ad->conn) {
        
        ad->DetachFromOwner(false);
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
        
        SetSyncError(c->adapter->conn->LastErrorMessage());
        
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
    delete c;                       
    if (!ad) return;

    
    
    if (issued && ad->conn) ad->conn->Close();

    
    ad->DetachFromOwner(issued);
}

} 




