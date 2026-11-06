
















#ifndef TT_DNS_RESOLVER_H
#define TT_DNS_RESOLVER_H

#include <string>
#include <vector>

#include <BC/BCSocket.h>
#include <BC/Config.h>

#include "VpnPolicy.h"

using namespace BC;





class DnsMessage
{
public:
    
    
    static std::vector<uint8_t> BuildQuery(const std::string& host,
                                           uint16_t qtype,
                                           uint16_t txid);

    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    static bool ParseResponse(const uint8_t* data, size_t len,
                              uint16_t expectTxid,
                              std::vector<BCSockAddrS>& outAddrs,
                              uint32_t& outMinTtl,
                              std::string& outErr,
                              const std::string& expectHost = std::string());

private:
    
    
    
    
    static size_t _SkipName(const uint8_t* data, size_t len, size_t offset);
};





struct DnsConfig
{
    TTVpnPolicy              policy    = TT_VPN_POLICY_UNSET;
    uint32_t                 ifIndex   = 0;       
    uint64_t                 androidNetHandle = 0; 
                                                    
                                                    
    std::vector<std::string> servers;             
    uint16_t                 serverPort = 53;     
                                                  
                                                  
    
    
    
    uint32_t                 timeoutMs = 1000;
    
    
    
    
    
    
    
    uint32_t                 secondAnswerGraceMs = 50;
    bool                     preferIpv6 = false;
    void*                    loggerCtx = nullptr;
};

struct DnsResult
{
    BCRESULT                 result = BC_R_FAILURE;
    std::vector<BCSockAddrS> addrs;               
    std::string              errMessage;
};





class DnsResolver
{
public:
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    static DnsResult Resolve(const std::string& host, const DnsConfig& cfg);

    
    static void ClearCache();

    
    static const char* kDefaultServers[2];   
};

#endif 




