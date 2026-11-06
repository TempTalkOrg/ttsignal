





#include "StdAfx.h"
#include "DnsResolver.h"

#include "INetworkPathMonitor.h"
#include "SocketPinner.h"
#include "TTErrors.h"
#include "Utils.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#define TT_CLOSESOCKET(fd)  closesocket((SOCKET)(fd))
#define TT_POLL(fds, n, ms) WSAPoll((fds), (n), (ms))
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#define TT_CLOSESOCKET(fd)  close(fd)
#define TT_POLL(fds, n, ms) poll((fds), (n), (ms))
#endif

namespace {


inline int _LastSocketError()
{
#if defined(_WIN32)
    return WSAGetLastError();
#else
    return errno;
#endif
}
}   





std::vector<uint8_t> DnsMessage::BuildQuery(const std::string& host,
                                            uint16_t qtype,
                                            uint16_t txid)
{
    std::vector<uint8_t> kEmpty;

    if (host.empty()) return kEmpty;

    
    
    std::vector<uint8_t> qname;
    size_t start = 0;
    while (start <= host.size())
    {
        size_t dot = host.find('.', start);
        size_t end = (dot == std::string::npos) ? host.size() : dot;
        size_t labelLen = end - start;

        if (labelLen == 0 || labelLen > 63) return kEmpty;

        qname.push_back((uint8_t)labelLen);
        for (size_t i = start; i < end; i++)
        {
            qname.push_back((uint8_t)host[i]);
        }

        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    qname.push_back(0x00);   

    
    if (qname.size() > 255) return kEmpty;

    std::vector<uint8_t> msg;
    msg.reserve(12 + qname.size() + 4);

    
    msg.push_back((uint8_t)(txid >> 8));
    msg.push_back((uint8_t)(txid & 0xFF));
    msg.push_back(0x01);
    msg.push_back(0x00);
    msg.push_back(0x00);
    msg.push_back(0x01);
    msg.push_back(0x00);
    msg.push_back(0x00);
    msg.push_back(0x00);
    msg.push_back(0x00);
    msg.push_back(0x00);
    msg.push_back(0x00);

    msg.insert(msg.end(), qname.begin(), qname.end());

    msg.push_back((uint8_t)(qtype >> 8));
    msg.push_back((uint8_t)(qtype & 0xFF));
    msg.push_back(0x00);
    msg.push_back(0x01);   

    return msg;
}

size_t DnsMessage::_SkipName(const uint8_t* data, size_t len, size_t offset)
{
    
    
    
    
    size_t consumed = 0;
    size_t pos      = offset;

    while (pos < len) {
        uint8_t b = data[pos];

        if ((b & 0xC0) == 0xC0) {
            
            if (pos + 1 >= len) return 0;
            size_t target = ((size_t)(b & 0x3F) << 8) | data[pos + 1];
            if (target >= len) return 0;   
            if (target == pos) return 0;   
            return consumed + 2;
        }
        if ((b & 0xC0) != 0x00) {
            return 0;   
        }
        if (b == 0) {
            return consumed + 1;   
        }
        if (pos + 1 + b > len) return 0;   
        pos      += 1 + b;
        consumed += 1 + b;
    }
    return 0;
}

namespace {

uint16_t _ReadU16(const uint8_t* data, size_t pos)
{
    return (uint16_t)((data[pos] << 8) | data[pos + 1]);
}

uint32_t _ReadU32(const uint8_t* data, size_t pos)
{
    return ((uint32_t)data[pos] << 24) | ((uint32_t)data[pos + 1] << 16) |
           ((uint32_t)data[pos + 2] << 8) | (uint32_t)data[pos + 3];
}







bool _DecodeQuestionName(const uint8_t* data, size_t len, size_t offset, std::string& outName)
{
    outName.clear();
    size_t pos = offset;
    int jumps = 0;
    const int kMaxJumps = 32;
    bool first = true;

    while (true) {
        if (pos >= len) return false;
        uint8_t b = data[pos];

        if ((b & 0xC0) == 0xC0) {
            if (pos + 1 >= len) return false;
            size_t target = ((size_t)(b & 0x3F) << 8) | data[pos + 1];
            if (target >= len) return false;
            if (++jumps > kMaxJumps) return false;
            pos = target;
            continue;
        }
        if ((b & 0xC0) != 0x00) return false;
        if (b == 0) break;   
        if (pos + 1 + (size_t)b > len) return false;

        if (!first) outName += '.';
        first = false;
        for (size_t i = 0; i < (size_t)b; i++) {
            outName += (char)tolower((unsigned char)data[pos + 1 + i]);
        }
        pos += 1 + b;
    }
    return true;
}

std::string _LowerNoTrailingDot(const std::string& host)
{
    std::string out = host;
    while (!out.empty() && out.back() == '.') out.pop_back();
    for (auto& c : out) c = (char)tolower((unsigned char)c);
    return out;
}

}   

bool DnsMessage::ParseResponse(const uint8_t* data, size_t len,
                               uint16_t expectTxid,
                               std::vector<BCSockAddrS>& outAddrs,
                               uint32_t& outMinTtl,
                               std::string& outErr,
                               const std::string& expectHost)
{
    outErr.clear();

    
    
    auto fail = [&](const std::string& msg) -> bool {
        outAddrs.clear();
        outErr = msg;
        return false;
    };

    if (data == nullptr || len < 12) {
        return fail("DNS 应答短于 12 字节 header");
    }

    if ((data[2] & 0x80) == 0) {
        
        
        return fail("DNS 应答 QR 位为 0（不是应答，可能是查询被环回或伪造）");
    }

    uint16_t gotTxid = _ReadU16(data, 0);
    if (gotTxid != expectTxid) {
        
        return fail("DNS 应答 txid 不匹配（可能是伪造应答）");
    }

    uint8_t rcode = data[3] & 0x0F;
    if (rcode != 0) {
        return fail("DNS 应答 RCODE=" + std::to_string((int)rcode) + "（非 0，查询失败）");
    }

    bool truncated = (data[2] & 0x02) != 0;

    uint16_t qdcount = _ReadU16(data, 4);
    uint16_t ancount = _ReadU16(data, 6);

    
    
    
    
    
    
    if (!expectHost.empty() && qdcount == 0) {
        return fail("DNS 应答 QDCOUNT=0，无法校验 question 域名（可能是伪造应答）");
    }

    size_t pos = 12;
    for (uint16_t i = 0; i < qdcount; i++) {
        if (i == 0 && !expectHost.empty()) {
            
            
            
            
            std::string qname;
            if (!_DecodeQuestionName(data, len, pos, qname)) {
                return fail("DNS 应答 question NAME 解码失败（可能是伪造应答）");
            }
            if (qname != _LowerNoTrailingDot(expectHost)) {
                return fail("DNS 应答 question 域名与查询不符（可能是伪造应答）: got=" + qname);
            }
        }

        size_t nameLen = _SkipName(data, len, pos);
        if (nameLen == 0) {
            return fail("DNS 应答 question NAME 越界或压缩指针非法");
        }
        pos += nameLen;
        if (pos + 4 > len) {
            return fail("DNS 应答 question 越界");
        }
        pos += 4;   
    }

    bool haveTtl = false;

    for (uint16_t i = 0; i < ancount; i++) {
        size_t nameLen = _SkipName(data, len, pos);
        if (nameLen == 0) {
            if (truncated) break;   
            return fail("DNS 应答 answer NAME 越界或压缩指针非法");
        }
        pos += nameLen;

        if (pos + 10 > len) {   
            if (truncated) break;
            return fail("DNS 应答 answer 记录头越界");
        }

        uint16_t type = _ReadU16(data, pos);
        pos += 2;
        pos += 2;   
        uint32_t ttl = _ReadU32(data, pos);
        pos += 4;
        uint16_t rdlen = _ReadU16(data, pos);
        pos += 2;

        if (pos + rdlen > len) {
            if (truncated) break;
            return fail("DNS 应答 RDATA 越界");
        }

        if (type == 1 && rdlen == 4) {
            struct in_addr in4;
            memcpy(&in4, data + pos, 4);
            BCSockAddrS sa;
            bc_sockaddr_fromin(&sa, &in4, 0);
            outAddrs.push_back(sa);
            if (!haveTtl || ttl < outMinTtl) outMinTtl = ttl;
            haveTtl = true;
        } else if (type == 28 && rdlen == 16) {
            struct in6_addr in6;
            memcpy(&in6, data + pos, 16);
            BCSockAddrS sa;
            bc_sockaddr_fromin6(&sa, &in6, 0);
            outAddrs.push_back(sa);
            if (!haveTtl || ttl < outMinTtl) outMinTtl = ttl;
            haveTtl = true;
        }
        

        pos += rdlen;
    }

    if (truncated && outAddrs.empty()) {
        return fail("DNS 应答被截断（TC 置位）且未解出任何地址");
    }

    return true;
}





const char* DnsResolver::kDefaultServers[2] = {"223.5.5.5", "8.8.8.8"};

namespace {

struct CacheEntry
{
    std::vector<BCSockAddrS>             addrs;
    std::chrono::steady_clock::time_point expireAt;
};

BCSpinMutex                     g_cacheLock;
std::map<std::string, CacheEntry> g_cache;

std::string _CacheKey(const std::string& host, TTVpnPolicy policy, uint32_t ifIndex)
{
    
    
    return host + "|" + std::to_string((int)policy) + "|" + std::to_string(ifIndex);
}

uint32_t _ClampTtl(uint32_t ttl)
{
    if (ttl < 10)  return 10;
    if (ttl > 600) return 600;
    return ttl;
}

bool _CacheLookup(const std::string& key, DnsResult& out)
{
    BCSpinMutex::Owner lock(g_cacheLock);

    auto it = g_cache.find(key);
    if (it == g_cache.end()) return false;

    if (std::chrono::steady_clock::now() >= it->second.expireAt) {
        g_cache.erase(it);
        return false;
    }

    out.result = BC_R_SUCCESS;
    out.addrs  = it->second.addrs;
    out.errMessage.clear();
    return true;
}

void _CacheStore(const std::string& key, const std::vector<BCSockAddrS>& addrs,
                  uint32_t ttlSeconds)
{
    CacheEntry entry;
    entry.addrs    = addrs;
    entry.expireAt = std::chrono::steady_clock::now() +
                     std::chrono::seconds(_ClampTtl(ttlSeconds));

    BCSpinMutex::Owner lock(g_cacheLock);
    g_cache[key] = entry;
}


bool _TryParseLiteral(const std::string& host, DnsResult& outResult)
{
    struct in_addr v4;
    if (inet_pton(AF_INET, host.c_str(), &v4) == 1) {
        BCSockAddrS sa;
        bc_sockaddr_fromin(&sa, &v4, 0);
        outResult.result = BC_R_SUCCESS;
        outResult.addrs.push_back(sa);
        outResult.errMessage.clear();
        return true;
    }

    struct in6_addr v6;
    if (inet_pton(AF_INET6, host.c_str(), &v6) == 1) {
        BCSockAddrS sa;
        bc_sockaddr_fromin6(&sa, &v6, 0);
        outResult.result = BC_R_SUCCESS;
        outResult.addrs.push_back(sa);
        outResult.errMessage.clear();
        return true;
    }

    return false;
}

void _SortAddrs(std::vector<BCSockAddrS>& addrs, bool preferIpv6)
{
    std::stable_sort(addrs.begin(), addrs.end(),
        [preferIpv6](const BCSockAddrS& a, const BCSockAddrS& b) {
            bool aV6 = (a.type.sa.sa_family == AF_INET6);
            bool bV6 = (b.type.sa.sa_family == AF_INET6);
            if (aV6 == bV6) return false;
            return preferIpv6 ? (aV6 && !bV6) : (!aV6 && bV6);
        });
}

uint16_t _RandomTxid()
{
    
    thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> dist(0, 65535);
    return (uint16_t)dist(rng);
}


DnsResult _ResolveViaSystem(const std::string& host, const DnsConfig& cfg, uint32_t& outTtl)
{
    DnsResult r;
    outTtl = 60;   

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* res = nullptr;
    int e = getaddrinfo(host.c_str(), nullptr, &hints, &res);
    if (e != 0) {
        r.result      = BC_R_DNS_FAILED;
        r.errMessage  = std::string("getaddrinfo 失败: ") + gai_strerror(e);
        return r;
    }

    for (struct addrinfo* p = res; p != nullptr; p = p->ai_next) {
        if (p->ai_family != AF_INET && p->ai_family != AF_INET6) continue;
        if ((size_t)p->ai_addrlen > sizeof(((BCSockAddrS*)nullptr)->type)) continue;

        BCSockAddrS sa;
        memset(&sa, 0, sizeof(sa));
        memcpy(&sa.type, p->ai_addr, p->ai_addrlen);
        sa.length = (unsigned int)p->ai_addrlen;
        r.addrs.push_back(sa);
    }
    freeaddrinfo(res);

    if (r.addrs.empty()) {
        r.result     = BC_R_DNS_FAILED;
        r.errMessage = "getaddrinfo 未返回任何地址";
        return r;
    }

    _SortAddrs(r.addrs, cfg.preferIpv6);
    r.result = BC_R_SUCCESS;
    return r;
}


DnsResult _ResolveViaUdp(const std::string& host, const DnsConfig& cfg, uint32_t& outTtl)
{
    DnsResult r;
    r.result = BC_R_FAILURE;
    outTtl   = 60;

    if (DnsMessage::BuildQuery(host, 1, 0).empty()) {
        r.errMessage = "非法域名，无法构造 DNS 查询: " + host;
        return r;
    }

    std::vector<std::string> defaultServers;
    const std::vector<std::string>* serversPtr = &cfg.servers;
    if (cfg.servers.empty()) {
        defaultServers.push_back(DnsResolver::kDefaultServers[0]);
        defaultServers.push_back(DnsResolver::kDefaultServers[1]);
        serversPtr = &defaultServers;
    }
    const std::vector<std::string>& servers = *serversPtr;

    std::string aggregateErr;

    for (size_t s = 0; s < servers.size(); s++) {
        const std::string& server = servers[s];

        struct in_addr  serverV4;
        struct in6_addr serverV6;
        bool isV6 = false;
        if (inet_pton(AF_INET, server.c_str(), &serverV4) == 1) {
            isV6 = false;
        } else if (inet_pton(AF_INET6, server.c_str(), &serverV6) == 1) {
            isV6 = true;
        } else {
            aggregateErr += server + ": 不是合法的 DNS server IP; ";
            continue;
        }

        int fd = (int)socket(isV6 ? AF_INET6 : AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) {
            aggregateErr += server + ": socket() 失败 errno=" + std::to_string(_LastSocketError()) + "; ";
            continue;
        }

        TTPinRequest pinReq;
        memset(&pinReq, 0, sizeof(pinReq));
        pinReq.fd               = fd;
        pinReq.ifIndex          = cfg.ifIndex;
        pinReq.androidNetHandle = cfg.androidNetHandle;
        pinReq.ipv6             = isV6 ? 1 : 0;
        pinReq.isTcp            = 0;
        pinReq.policy           = cfg.policy;
        pinReq.loggerCtx        = cfg.loggerCtx;

        char pinMethod[32] = {0};
        int  pinErrno      = 0;
        TTPinResult pinResult = tt_socket_pin(&pinReq, pinMethod, sizeof(pinMethod), &pinErrno);

        
        
        
        
        
        
        
        bool pinOk = (cfg.policy == TT_VPN_POLICY_FORCE_PHYSICAL)
                         ? (pinResult == TT_PIN_OK)
                         : (pinResult == TT_PIN_OK || pinResult == TT_PIN_NOT_NEEDED);
        if (!pinOk) {
            
            
            
            TT_CLOSESOCKET(fd);
            aggregateErr += server + ": 绑定物理网卡失败 (result=" +
                std::to_string((int)pinResult) + ", errno=" + std::to_string(pinErrno) + "); ";
            r.errMessage = aggregateErr;
            return r;
        }

        BCSockAddrS serverAddr;
        if (isV6) {
            bc_sockaddr_fromin6(&serverAddr, &serverV6, cfg.serverPort);
        } else {
            bc_sockaddr_fromin(&serverAddr, &serverV4, cfg.serverPort);
        }

        
        
        
        
        if (connect(fd, &serverAddr.type.sa, (socklen_t)serverAddr.length) != 0) {
            TT_CLOSESOCKET(fd);
            aggregateErr += server + ": connect() 失败 errno=" +
                std::to_string(_LastSocketError()) + "; ";
            continue;
        }

        uint16_t txidA    = _RandomTxid();
        uint16_t txidAAAA = _RandomTxid();
        if (txidAAAA == txidA) txidAAAA = (uint16_t)(txidAAAA ^ 0x1);

        std::vector<uint8_t> queryA    = DnsMessage::BuildQuery(host, 1, txidA);
        std::vector<uint8_t> queryAAAA = DnsMessage::BuildQuery(host, 28, txidAAAA);

        ssize_t sentA    = send(fd, (const char*)queryA.data(), queryA.size(), 0);
        ssize_t sentAAAA = send(fd, (const char*)queryAAAA.data(), queryAAAA.size(), 0);

        
        
        bool gotA    = (sentA < 0);
        bool gotAAAA = (sentAAAA < 0);

        std::vector<BCSockAddrS> gathered;
        uint32_t minTtl  = 0;
        bool     haveTtl = false;
        std::string serverErr;

        auto deadline = std::chrono::steady_clock::now() +
                         std::chrono::milliseconds(cfg.timeoutMs);

        
        bool                                  graceArmed = false;
        std::chrono::steady_clock::time_point graceDeadline;

        uint8_t buf[512];
        while (!gotA || !gotAAAA) {
            auto now = std::chrono::steady_clock::now();

            if (!graceArmed && !gathered.empty()) {
                graceArmed    = true;
                graceDeadline = now + std::chrono::milliseconds(cfg.secondAnswerGraceMs);
            }

            
            
            auto effective = deadline;
            if (graceArmed && graceDeadline < effective) effective = graceDeadline;

            if (now >= effective) {
                
                
                if (!graceArmed) serverErr = "超时";
                break;
            }
            int remainingMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
                effective - now).count();
            if (remainingMs < 0) remainingMs = 0;

            struct pollfd pfd;
            pfd.fd      = fd;
            pfd.events  = POLLIN;
            pfd.revents = 0;
            int pr = TT_POLL(&pfd, 1, remainingMs);
            if (pr <= 0) {
                
                if (!graceArmed) serverErr = "超时";
                break;
            }
            if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                
                
                
                
                serverErr = "socket 出错 (revents=" + std::to_string(pfd.revents) + ")";
                break;
            }
            if (!(pfd.revents & POLLIN)) continue;

            ssize_t n = recv(fd, (char*)buf, sizeof(buf), 0);
            if (n <= 0) continue;

            
            
            
            
            uint16_t gotTxidHdr = (n >= 2) ? _ReadU16(buf, 0) : 0;

            if (!gotA && gotTxidHdr == txidA) {
                std::vector<BCSockAddrS> tmpAddrs;
                uint32_t tmpTtl = 0;
                std::string tmpErr;
                gotA = true;
                if (DnsMessage::ParseResponse(buf, (size_t)n, txidA, tmpAddrs, tmpTtl, tmpErr, host)) {
                    for (auto& a : tmpAddrs) gathered.push_back(a);
                    if (!tmpAddrs.empty()) {
                        if (!haveTtl || tmpTtl < minTtl) minTtl = tmpTtl;
                        haveTtl = true;
                    }
                } else {
                    
                    
                    serverErr = tmpErr;
                }
                continue;
            }
            if (!gotAAAA && gotTxidHdr == txidAAAA) {
                std::vector<BCSockAddrS> tmpAddrs;
                uint32_t tmpTtl = 0;
                std::string tmpErr;
                gotAAAA = true;
                if (DnsMessage::ParseResponse(buf, (size_t)n, txidAAAA, tmpAddrs, tmpTtl, tmpErr, host)) {
                    for (auto& a : tmpAddrs) gathered.push_back(a);
                    if (!tmpAddrs.empty()) {
                        if (!haveTtl || tmpTtl < minTtl) minTtl = tmpTtl;
                        haveTtl = true;
                    }
                } else {
                    serverErr = tmpErr;
                }
                continue;
            }
            
        }

        TT_CLOSESOCKET(fd);

        if (!gathered.empty()) {
            _SortAddrs(gathered, cfg.preferIpv6);
            r.result = BC_R_SUCCESS;
            r.addrs  = gathered;
            r.errMessage.clear();
            if (haveTtl) outTtl = minTtl;
            return r;
        }

        aggregateErr += server + ": " + (serverErr.empty() ? std::string("无应答") : serverErr) + "; ";
    }

    r.result     = BC_R_FAILURE;
    r.errMessage = aggregateErr.empty() ? "没有可用的 DNS server" : aggregateErr;
    return r;
}

}   

DnsResult DnsResolver::Resolve(const std::string& host, const DnsConfig& cfgIn)
{
    DnsConfig cfg = cfgIn;
    if (cfg.policy == TT_VPN_POLICY_UNSET) {
        cfg.policy = tt_vpn_policy_platform_default();
    }

    DnsResult literal;
    if (_TryParseLiteral(host, literal)) {
        return literal;
    }

    
    
    
    
    
    
    
    
    
#if defined(TT_HAS_PATH_MONITOR)
    if (cfg.policy != TT_VPN_POLICY_OS && cfg.ifIndex == 0) {
        int64_t detected = tt_netmon_query_default_ifindex_ex((int)cfg.policy);
        if (detected > 0) {
            cfg.ifIndex = (uint32_t)detected;
        }
        
        
        
    }
#endif

    
    
    
    
    
    
    if (cfg.policy == TT_VPN_POLICY_FORCE_PHYSICAL) {
        bool haveInterface = (cfg.ifIndex != 0);
#ifdef OS_ANDROID
        haveInterface = haveInterface || (cfg.androidNetHandle != 0);
#endif
        if (!haveInterface) {
            DnsResult r;
            r.result     = BC_R_NO_PHYSICAL_INTERFACE;
            r.errMessage = "force-physical 找不到可用物理网卡（ifIndex/androidNetHandle 均为 0）";
            return r;
        }
    }

    std::string key = _CacheKey(host, cfg.policy, cfg.ifIndex);
    DnsResult cached;
    if (_CacheLookup(key, cached)) {
        return cached;
    }

    uint32_t ttlSeconds = 60;
    DnsResult r;
    bool usedFallback = false;

    if (cfg.policy == TT_VPN_POLICY_OS) {
        r = _ResolveViaSystem(host, cfg, ttlSeconds);
    } else {
        r = _ResolveViaUdp(host, cfg, ttlSeconds);
        if (r.result != BC_R_SUCCESS) {
            if (cfg.policy == TT_VPN_POLICY_FORCE_PHYSICAL) {
                
                
                r.result = BC_R_DNS_FAILED;
                return r;
            }

            LogQ(cfg.loggerCtx, _WARN_,
                "DNS: prefer-physical 自建查询失败（%s），回落系统 getaddrinfo。"
                "TUN 全局代理下解析结果可能是 fake-IP。", r.errMessage.c_str());
            r = _ResolveViaSystem(host, cfg, ttlSeconds);
            usedFallback = true;
        }
    }

    
    
    
    if (!usedFallback && r.result == BC_R_SUCCESS && !r.addrs.empty()) {
        _CacheStore(key, r.addrs, ttlSeconds);
    }
    return r;
}

void DnsResolver::ClearCache()
{
    BCSpinMutex::Owner lock(g_cacheLock);
    g_cache.clear();
}




