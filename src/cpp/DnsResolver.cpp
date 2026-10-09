///////////////////////////////////////////////////////////////////////////////
// file   : DnsResolver.cpp
// author : anto
//
// 绕过 VPN 的 DNS 解析实现。详见 DnsResolver.h 顶部注释。
///////////////////////////////////////////////////////////////////////////////

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
// Windows 的 socket API 不设置 errno，取错误码要用 WSAGetLastError()。统一
// 走这个 helper，避免各处散落 #ifdef。
inline int _LastSocketError()
{
#if defined(_WIN32)
    return WSAGetLastError();
#else
    return errno;
#endif
}
}   // namespace

///////////////////////////////////////////////////////////////////////////////
// DnsMessage —— 报文编解码，纯函数
///////////////////////////////////////////////////////////////////////////////

std::vector<uint8_t> DnsMessage::BuildQuery(const std::string& host,
                                            uint16_t qtype,
                                            uint16_t txid)
{
    std::vector<uint8_t> kEmpty;

    if (host.empty()) return kEmpty;

    // 把 host 按 '.' 切成 label，逐段编码为 len + bytes。空 label（连续的点、
    // 前导/尾随点）与超过 63 字节的 label 一律拒绝。
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
    qname.push_back(0x00);   // root label

    // RFC 1035：编码后的 QNAME（含 root label）不得超过 255 字节。
    if (qname.size() > 255) return kEmpty;

    std::vector<uint8_t> msg;
    msg.reserve(12 + qname.size() + 4);

    // Header：txid 大端、flags=0x0100（RD=1）、QDCOUNT=1，其余计数为 0。
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
    msg.push_back(0x01);   // QCLASS=IN

    return msg;
}

size_t DnsMessage::_SkipName(const uint8_t* data, size_t len, size_t offset)
{
    // 返回该名字在"当前位置"占用的字节数（遇到指针即结束，指针本身占 2 字节）。
    // 越界或 label 长度非法返回 0。这里只跳过、不解析——我们不需要 answer 的
    // NAME 内容，只需要正确越过它拿到后面的 TYPE/CLASS/TTL/RDLENGTH，因此不必
    // 真的跟随指针，成环风险天然消失；但指针目标的合法性仍要校验。
    size_t consumed = 0;
    size_t pos      = offset;

    while (pos < len) {
        uint8_t b = data[pos];

        if ((b & 0xC0) == 0xC0) {
            // 压缩指针，占 2 字节且一定是名字的结尾。
            if (pos + 1 >= len) return 0;
            size_t target = ((size_t)(b & 0x3F) << 8) | data[pos + 1];
            if (target >= len) return 0;   // 越界指针
            if (target == pos) return 0;   // 自环
            return consumed + 2;
        }
        if ((b & 0xC0) != 0x00) {
            return 0;   // 0x40 / 0x80 是保留的 label 类型，视为畸形
        }
        if (b == 0) {
            return consumed + 1;   // root label，名字结束
        }
        if (pos + 1 + b > len) return 0;   // label 越界
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

// 解码一个（可能被压缩的）域名的完整文本内容，跟随压缩指针，小写化，用于
// 跟 expectHost 做内容比对。与 DnsMessage::_SkipName 刻意不同——_SkipName
// 只关心"跳过多少字节"，不需要跟随指针；这里的场景不一样：我们要拿到 question
// 里的真实域名文本去比对，必须真的跟随并拼出内容。用 kMaxJumps 给跳转次数
// 设上限（每次跳转都要先做越界校验），无论指针是否成环，跳转计数单调递增，
// 一定会在 kMaxJumps 次之内终止，不会死循环。
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
        if (b == 0) break;   // root label，结束
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

}   // namespace

bool DnsMessage::ParseResponse(const uint8_t* data, size_t len,
                               uint16_t expectTxid,
                               std::vector<BCSockAddrS>& outAddrs,
                               uint32_t& outMinTtl,
                               std::string& outErr,
                               const std::string& expectHost)
{
    outErr.clear();

    // 契约：失败时 outAddrs 必须是空的——这个小 helper 保证每一条失败路径都
    // 清掉可能已经解析出的地址，不留半成品给调用方。
    auto fail = [&](const std::string& msg) -> bool {
        outAddrs.clear();
        outErr = msg;
        return false;
    };

    if (data == nullptr || len < 12) {
        return fail("DNS 应答短于 12 字节 header");
    }

    if ((data[2] & 0x80) == 0) {
        // QR=0 表示这是一个"查询"而不是"应答"——正常服务器不会这样回，但把
        // 我们自己发出的查询原样环回（同一台主机、同一个 txid）就会长这样。
        return fail("DNS 应答 QR 位为 0（不是应答，可能是查询被环回或伪造）");
    }

    uint16_t gotTxid = _ReadU16(data, 0);
    if (gotTxid != expectTxid) {
        // 防 off-path 伪造应答：txid 必须与本机发出的查询一致。
        return fail("DNS 应答 txid 不匹配（可能是伪造应答）");
    }

    uint8_t rcode = data[3] & 0x0F;
    if (rcode != 0) {
        return fail("DNS 应答 RCODE=" + std::to_string((int)rcode) + "（非 0，查询失败）");
    }

    bool truncated = (data[2] & 0x02) != 0;

    uint16_t qdcount = _ReadU16(data, 4);
    uint16_t ancount = _ReadU16(data, 6);

    // 反 QDCOUNT=0 绕过：question 校验原来写在 "for (i < qdcount)" 循环体
    // 内，qdcount==0 时循环一次都不执行，expectHost 形同虚设，answer 照收。
    // 攻击者猜中 txid 后只要把 question 段整个删掉（QDCOUNT 置 0）就能绕过
    // 域名校验、投毒任意地址。这里在进入循环之前就单独判一次：合法的
    // RCODE=0 应答一定会回显 question 段，QDCOUNT=0 本身就是畸形/伪造信号，
    // 只要调用方要求了 expectHost 校验就必须拒绝，不能指望循环体来做这件事。
    if (!expectHost.empty() && qdcount == 0) {
        return fail("DNS 应答 QDCOUNT=0，无法校验 question 域名（可能是伪造应答）");
    }

    size_t pos = 12;
    for (uint16_t i = 0; i < qdcount; i++) {
        if (i == 0 && !expectHost.empty()) {
            // 第二道防线：txid 之外，question 里问的域名也必须是我们查询的
            // 那个。off-path 攻击者哪怕猜中了 txid，一般也猜不到我们此刻正在
            // 查询哪个域名——这一条能拦下"txid 对上了但答案是伪造的"这种
            // 注入（例如把 answer 换成攻击者想要的任意 IP）。
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
        pos += 4;   // QTYPE + QCLASS
    }

    bool haveTtl = false;

    for (uint16_t i = 0; i < ancount; i++) {
        size_t nameLen = _SkipName(data, len, pos);
        if (nameLen == 0) {
            if (truncated) break;   // TC 位下按"结果不完整"处理，见下方收尾逻辑
            return fail("DNS 应答 answer NAME 越界或压缩指针非法");
        }
        pos += nameLen;

        if (pos + 10 > len) {   // TYPE(2) + CLASS(2) + TTL(4) + RDLENGTH(2)
            if (truncated) break;
            return fail("DNS 应答 answer 记录头越界");
        }

        uint16_t type = _ReadU16(data, pos);
        pos += 2;
        pos += 2;   // CLASS，不校验
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
        // 其余类型（CNAME 等）或 RDLENGTH 与类型不符：跳过，不当成地址。

        pos += rdlen;
    }

    if (truncated && outAddrs.empty()) {
        return fail("DNS 应答被截断（TC 置位）且未解出任何地址");
    }

    return true;
}

///////////////////////////////////////////////////////////////////////////////
// DnsResolver —— 缓存 + 三档策略
///////////////////////////////////////////////////////////////////////////////

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
    // key 里必须含 policy 和 ifIndex：同一域名在物理网卡和 VPN 下解析结果
    // 不同，混用会导致极难排查的串味问题。
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

// host 本身是 IP 字面量时直接返回它，不查询。
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
    // Resolve 会被多个 task 线程并发调用，用 thread_local 引擎避免共享状态。
    thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> dist(0, 65535);
    return (uint16_t)dist(rng);
}

// policy == OS：直接走系统 getaddrinfo，不做绕过。
DnsResult _ResolveViaSystem(const std::string& host, const DnsConfig& cfg, uint32_t& outTtl)
{
    DnsResult r;
    outTtl = 60;   // getaddrinfo 不提供 TTL，取一个保守的默认值

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

// policy != OS：自建 UDP 查询，socket 先过 SocketPinner 绑物理网卡。
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

        // force-physical 下"用真实 IP 出网"是硬约束：TT_PIN_NOT_NEEDED 也不
        // 能放行——它的字面意思是"调用方没要求绑定"（ifIndex/androidNetHandle
        // 均为 0），代表 socket 完全没绑，查询会走系统默认路由，TUN 全局模式
        // 下那就是隧道本身。只有 TT_PIN_OK 才代表这次查询真的从物理网卡出去。
        // prefer-physical 没有这个硬约束，NOT_NEEDED 允许继续（对应
        // VpnPolicy.h 里"找不到物理网卡可回落隧道"的既有语义），但真正的绑定
        // 失败（FAILED/UNSUPPORTED）依然要放弃，交给上层回落 getaddrinfo。
        bool pinOk = (cfg.policy == TT_VPN_POLICY_FORCE_PHYSICAL)
                         ? (pinResult == TT_PIN_OK)
                         : (pinResult == TT_PIN_OK || pinResult == TT_PIN_NOT_NEEDED);
        if (!pinOk) {
            // 绑定这一步只取决于 ifIndex/androidNetHandle，与具体 server 无
            // 关：失败一次，其余 server 也一定失败，直接放弃自建查询，交由
            // 上层决定是否回落。
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

        // connect() 这个 UDP socket：让内核只接受来自 server 这个地址+端口的
        // 包，过滤掉任何其它主机（哪怕猜中了 txid）发到这个临时端口的数据。
        // 这不能防真正能伪造源 IP 的攻击者，但能挡掉"任意主机误发/恶意发到
        // 这个端口"的廉价注入，是 txid/question 校验之外的又一层过滤。
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

        // 发送失败的那一路直接视为"已完成"（不再等待），避免死等一个永远不会
        // 到来的应答；两路都失败时下面的 while 循环不会执行，直接判定超时。
        bool gotA    = (sentA < 0);
        bool gotAAAA = (sentAAAA < 0);

        std::vector<BCSockAddrS> gathered;
        uint32_t minTtl  = 0;
        bool     haveTtl = false;
        std::string serverErr;

        auto deadline = std::chrono::steady_clock::now() +
                         std::chrono::milliseconds(cfg.timeoutMs);

        // 一旦有一路带回了地址，另一路就只剩 secondAnswerGraceMs 的宽限期。
        bool                                  graceArmed = false;
        std::chrono::steady_clock::time_point graceDeadline;

        uint8_t buf[512];
        while (!gotA || !gotAAAA) {
            auto now = std::chrono::steady_clock::now();

            if (!graceArmed && !gathered.empty()) {
                graceArmed    = true;
                graceDeadline = now + std::chrono::milliseconds(cfg.secondAnswerGraceMs);
            }

            // 手上有地址时按宽限期收尾，没有就老实等满 timeoutMs——两路都还空着
            // 的时候提前放弃，只会把"慢一点的 server"误判成"不可用的 server"。
            auto effective = deadline;
            if (graceArmed && graceDeadline < effective) effective = graceDeadline;

            if (now >= effective) {
                // 有地址时这个 break 是正常收尾，不是失败，别污染诊断信息：
                // 下面 gathered 非空会直接走成功路径。
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
                // 宽限期到点是最常见的退出口（另一路确实丢了），同上，不算失败。
                if (!graceArmed) serverErr = "超时";
                break;
            }
            if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                // 只收到错误类事件、没有 POLLIN：socket 已经坏了（比如
                // ICMP port-unreachable 反映到 connect() 过的 UDP socket
                // 上），继续 poll 只会一直立刻返回同样的 revents，空转到
                // deadline。直接判失败，不再等。
                serverErr = "socket 出错 (revents=" + std::to_string(pfd.revents) + ")";
                break;
            }
            if (!(pfd.revents & POLLIN)) continue;

            ssize_t n = recv(fd, (char*)buf, sizeof(buf), 0);
            if (n <= 0) continue;

            // 先在本层用 txid 判断这份应答是回给 A 查询还是 AAAA 查询——
            // 只有 txid 对上了，才认为"这条查询已经有结果了"（不管
            // ParseResponse 最终判它成功还是失败，都不用再等这一路）。
            // txid 都对不上的包（重复/迟到/伪造）直接忽略，继续等真正的应答。
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
                    // 真实失败原因（RCODE、question 域名不符等）比笼统的
                    // "超时"更有诊断价值，覆盖掉之前可能设置的 serverErr。
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
            // 既不匹配 A 也不匹配 AAAA 的 txid：忽略（迟到的重复包或伪造包）。
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

}   // namespace

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

    // ifIndex 为 0（调用方没有指定绑哪块网卡）且不是 os 策略时，自己去问一次
    // 系统当前的物理网卡出口——跟 SMPConnector.cpp 里 force-physical 分支
    // 的既有用法（tt_netmon_query_default_ifindex_ex）保持一致，不要为
    // DnsResolver 另发明一套探测机制。只有定义了 TT_HAS_PATH_MONITOR 的构建
    // （NAPI/iOS）才链接了这个符号；Android JNI 完全不定义它，只能依赖调用方
    // 通过 cfg.androidNetHandle 传入 Java NetworkCallback 拿到的 handle。
    //
    // 探测出来的 ifIndex 会写回 cfg，一并体现在下面的缓存 key 里——这正是
    // 期望行为：物理网卡切换后自动探测到新 ifIndex，不会复用旧网卡下的缓存。
#if defined(TT_HAS_PATH_MONITOR)
    if (cfg.policy != TT_VPN_POLICY_OS && cfg.ifIndex == 0) {
        int64_t detected = tt_netmon_query_default_ifindex_ex((int)cfg.policy);
        if (detected > 0) {
            cfg.ifIndex = (uint32_t)detected;
        }
        // 探测不到（<=0）：prefer-physical 允许继续（ifIndex 仍是 0，下面会
        // 走 TT_PIN_NOT_NEEDED 不绑定直接查询，失败再回落 getaddrinfo，等价
        // 于"找不到物理网卡就回落隧道"）；force-physical 由下面的硬校验兜底。
    }
#endif

    // force-physical 的硬约束：没有任何可绑定的物理网卡标识（ifIndex 与
    // androidNetHandle 均为 0）就必须直接失败，绝不能让 socket 悄悄地不绑定
    // 就把查询发出去——那样查询会走系统默认路由，TUN 全局模式下就是隧道本身，
    // 而"绕过 VPN"正是这个模块存在的唯一理由。BC_R_NO_PHYSICAL_INTERFACE 由
    // deps/env/src/BC/Config.h 提供（值 64，跨语言契约，不在 TTErrors.h 里
    // 重新定义），与 SMPConnector.cpp 的 force-physical 分支用同一个错误码。
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
                // force-physical 不回落：绕过 VPN 是硬约束，宁可失败也不要
                // 让业务在不知情的情况下把流量走进隧道。
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

    // 回落路径（prefer-physical 自建查询失败后走的 getaddrinfo）不缓存：这个
    // 结果很可能是 fake-IP（TUN 全局代理劫持），一旦写进 policy=prefer-physical
    // 这个 key，物理网卡恢复之后仍会在 TTL 窗口内继续命中这份脏缓存。
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

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
