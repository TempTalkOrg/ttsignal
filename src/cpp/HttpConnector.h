















































































































#ifndef TT_HTTP_CONNECTOR_H
#define TT_HTTP_CONNECTOR_H

#include <condition_variable>
#include <memory>
#include <mutex>
#include <map>
#include <set>
#include <string>

#include <BC/BCFCodec.h>

#include "LLHTTPParser.h"
#include "TcpChannel.h"
#include "TTErrors.h"





struct HttpRequest
{
    std::string   method    = "GET";
    std::string   url;                    
    HttpHeaderMap headers;
    std::string   body;
    uint32_t      timeoutMs = 10000;      
    std::string   resolvedIp;             
};

struct HttpResponse
{
    int           status       = 0;
    
    
    
    
    
    
    std::string   reason;
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    HttpHeaderMap headers;
    std::string   body;
    
    std::string   peerIp;
    uint32_t      boundIfIndex = 0;
    std::string   pinMethod;
};





struct HttpUrlParts
{
    std::string scheme;        
    std::string host;          
    std::string hostHeader;    
    uint16_t    port = 0;
    std::string target;        
    bool        tls  = false;
};





class IHttpRequestHandler
{
public:
    IHttpRequestHandler() {}
    virtual ~IHttpRequestHandler() {}

    virtual void OnHttpResponse(const HttpResponse& resp)                 = 0;
    virtual void OnHttpError(BCRESULT result, const std::string& message) = 0;
};

class IHttpConnectorHandler
{
public:
    IHttpConnectorHandler() {}
    virtual ~IHttpConnectorHandler() {}

    virtual void OnLog(int level, LPCSTR msg) = 0;
    virtual void OnClosed()                   = 0;
};





class HttpConnection;
class PooledChannel;












struct HttpLogSink
{
    
    
    
    
    
    
    
    std::mutex             lock;
    IHttpConnectorHandler* handler = NULL;
};




































struct HttpConnectorState
{
    std::recursive_mutex        lock;
    std::condition_variable_any cv;

    
    std::set<HttpConnection*>   conns;
    
    
    std::set<HttpConnection*>   destroying;

    
    
    
    
    
    
    
    
    
    
    
    std::map<std::string, std::vector<PooledChannel*> > idle;
    
    
    size_t                      max_idle_per_key = 4;
    
    
    
    
    
    
    
    size_t                      idle_destroying = 0;
    
    
    
    
    
    
    
    uint32_t                    idle_timeout_ms = 60 * 60 * 1000;   

    IHttpConnectorHandler*      handler         = NULL;
    bool                        closing         = false;
    bool                        closed_notified = false;
    
    
    
    bool                        abandoned       = false;
    
    
    
    uint32_t                    callbacks_in_flight = 0;

    
    void*                       logger_ctx      = NULL;
    bool                        own_logger      = false;
    HttpLogSink*                log_sink        = NULL;

    ~HttpConnectorState();
};

class HttpConnector
{
public:
    HttpConnector();
    ~HttpConnector();

    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    BCRESULT Create(BCFObject* pConfig, IHttpConnectorHandler* handler);

    
    
    
    
    
    
    
    BCRESULT Request(const HttpRequest& req, IHttpRequestHandler* handler,
                     std::string* outErrMessage = NULL);

    
    void     Close();

    
    
    
    
    
    TTVpnPolicy      EffectivePolicy()  const { return policy_; }
    size_t           MaxResponseBytes() const { return max_response_bytes_; }
    uint32_t         DrainTimeoutMs()   const { return drain_timeout_ms_; }
    
    size_t           MaxIdleConnections() const;
    uint32_t         IdleTimeoutMs()      const;

    
    
    
    
    
    void             SetIdleTimeoutMs(uint32_t ms);
    const DnsConfig& DnsSettings()      const { return dns_cfg_; }
    const TlsConfig& TlsSettings()      const { return tls_cfg_; }

    
    
    

    
    
    
    
    static bool ParseUrl(const std::string& url, HttpUrlParts& out,
                         std::string& outErr);

    
    
    
    
    
    static bool BuildRequestText(const HttpRequest& req,
                                 const HttpUrlParts& parts,
                                 std::string& out,
                                 std::string& outErr,
                                 bool keepAlive = false);

private:
    DECLARE_NO_COPY_CLASS(HttpConnector);
    friend class HttpConnection;

private:
    
    std::shared_ptr<HttpConnectorState> state_;

    void*                       logger_ctx_    = NULL;   

    TTVpnPolicy                 policy_        = TT_VPN_POLICY_UNSET;
    uint64_t                    android_net_handle_ = 0;
    DnsConfig                   dns_cfg_;
    TlsConfig                   tls_cfg_;
    size_t                      max_response_bytes_ = 8 * 1024 * 1024;
    uint32_t                    drain_timeout_ms_   = 30000;
    uint64_t                    bad_drain_timeout_  = 0;   
    uint64_t                    bad_idle_timeout_   = 0;   
    bool                        created_       = false;

    static void _LogCallback(void* data, int level, LPCSTR msg);
};

#endif 




