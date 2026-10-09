///////////////////////////////////////////////////////////////////////////////
// file   : DnsResolver.h
// author : anto
//
// 绕过 VPN 的 DNS 解析。
//
// 为什么不能只用 getaddrinfo：在 Clash / Surge / mihomo 的 TUN 全局模式下，
// getaddrinfo 走系统 DNS，请求被 VPN 劫持；fake-IP 模式会返回 198.18.0.0/15
// 段的假地址。此时即使把 TCP socket 硬绑到物理网卡，目标 IP 本身是假的，
// 物理网卡上根本没有到 198.18.x.x 的路由，连接直接黑洞。
//
// 所以"用真实 IP 出网"必须从 DNS 开始就绕过 VPN，只绑 TCP 是不够的。
//
// ⚠️ 线程约定：DnsResolver::Resolve 是同步阻塞调用（内部会 sendto/poll/recvfrom
// 直到超时或拿到应答），设计上跑在 BCTaskMgr 线程池里，不能在事件循环线程
// （Runtime 的 poll 线程）上直接调用，否则会卡住整个引擎的 IO 处理。
///////////////////////////////////////////////////////////////////////////////

#ifndef TT_DNS_RESOLVER_H
#define TT_DNS_RESOLVER_H

#include <string>
#include <vector>

#include <BC/BCSocket.h>
#include <BC/Config.h>

#include "VpnPolicy.h"

using namespace BC;

///////////////////////////////////////////////////////////////////////////////
// class : DnsMessage —— 纯函数，无 IO，可单测
///////////////////////////////////////////////////////////////////////////////

class DnsMessage
{
public:
    // 构造一个标准递归查询。qtype: 1=A, 28=AAAA。
    // host 非法（空、label > 63 字节、总长 > 253 字节）时返回空 vector。
    static std::vector<uint8_t> BuildQuery(const std::string& host,
                                           uint16_t qtype,
                                           uint16_t txid);

    // 解析应答，抽出 A/AAAA 地址。
    //
    // 返回 false 的情形：报文短于 header、QR 位为 0（不是应答）、txid 不匹配、
    // RCODE 非 0、压缩指针越界或成环、answer section 越界、question 域名与
    // expectHost 不符。outErr 说明具体原因。
    //
    // ⚠️ 契约：返回 false 时 outAddrs 保证为空（哪怕在失败前已经解析出过若干
    // 地址，也会在返回前清空）——调用方不需要、也不应该在失败时读取 outAddrs
    // 里的残留内容。
    //
    // outMinTtl 取所有被采纳记录 TTL 的最小值。没有地址记录时不修改它。
    // 非 A/AAAA 的记录（CNAME 等）被跳过，不算失败。
    // RDLENGTH 与记录类型不符的记录被跳过。
    //
    // expectHost：可选。非空时校验 question section 第一个问题的 QNAME
    // （大小写不敏感、忽略尾部'.'）是否与它一致，不一致视为伪造应答而拒绝。
    // 这是 txid 之外的第二道防线——off-path 攻击者哪怕猜中 16 位 txid，也很
    // 难同时猜中我们正在查询的具体域名。调用方（_ResolveViaUdp）总是传入真实
    // 查询的 host；单测里留空是因为只想测纯粹的报文结构解析。
    static bool ParseResponse(const uint8_t* data, size_t len,
                              uint16_t expectTxid,
                              std::vector<BCSockAddrS>& outAddrs,
                              uint32_t& outMinTtl,
                              std::string& outErr,
                              const std::string& expectHost = std::string());

private:
    // 跳过一个（可能被压缩的）域名，返回它在报文中占用的字节数。
    // 失败（越界 / 成环）返回 0。只跳过、不解析、不跟随指针——只在遇到指针的
    // 那一步校验一次目标偏移合法（不越界、不指向自己），不需要 maxJumps 之类
    // 的跳转计数器，成环风险因为"根本不跟随"而天然消失。
    static size_t _SkipName(const uint8_t* data, size_t len, size_t offset);
};

///////////////////////////////////////////////////////////////////////////////
// struct : DnsConfig / DnsResult
///////////////////////////////////////////////////////////////////////////////

struct DnsConfig
{
    TTVpnPolicy              policy    = TT_VPN_POLICY_UNSET;
    uint32_t                 ifIndex   = 0;       // policy == OS 时忽略
    uint64_t                 androidNetHandle = 0; // Android 用 net handle 而非 ifIndex，
                                                    // 语义与 TTPinRequest::androidNetHandle
                                                    // 一致（透传给 tt_socket_pin）
    std::vector<std::string> servers;             // 空则用默认公共 DNS
    uint16_t                 serverPort = 53;     // 所有 server 共用。改它的场景
                                                  // 是本机跑了 dnsmasq/DoH 代理
                                                  // 之类监听非标端口的解析器
    // 单台 server 判死的门槛。默认列表有两台，全哑掉的最坏情况是 2 × 这个值，
    // 所以它不能定得太宽松——一次 HTTP 请求等不起 4 秒。注意它跟下面的宽限期
    // 是两回事：宽限期只在已经拿到地址之后才收紧等待，两路都空手时仍然等满。
    uint32_t                 timeoutMs = 1000;
    // A 与 AAAA 是一起发出去的两路查询。其中一路已经带着地址回来之后，另一路
    // 最多再等这么久——它要么马上也到（正常情况下两路前后脚返回，这段宽限期
    // 一秒都用不满），要么就是丢了。为它死等满 timeoutMs 换不来任何东西：手上
    // 的地址已经够建连接了。实测 AAAA 应答被中间设备吞掉时，这一条把首次解析
    // 从 ~2000ms 压到 ~50ms。
    //
    // 置 0 = 不等：哪一路先带回地址就用哪一路的结果。
    uint32_t                 secondAnswerGraceMs = 50;
    bool                     preferIpv6 = false;
    void*                    loggerCtx = nullptr;
};

struct DnsResult
{
    BCRESULT                 result = BC_R_FAILURE;
    std::vector<BCSockAddrS> addrs;               // 按 preferIpv6 排序
    std::string              errMessage;
};

///////////////////////////////////////////////////////////////////////////////
// class : DnsResolver
///////////////////////////////////////////////////////////////////////////////

class DnsResolver
{
public:
    // ⚠️ 同步阻塞调用，必须在 BCTaskMgr 线程池里执行，不能在事件循环线程调用。
    //
    // policy == OS              : 直接 getaddrinfo
    // policy == PREFER_PHYSICAL : 先试自建查询；绑定失败或全部 server 超时
    //                             则回落 getaddrinfo 并打 _WARN_
    // policy == FORCE_PHYSICAL  : 只走自建查询，不回落。失败返回 BC_R_DNS_FAILED；
    //                             找不到可绑定的物理网卡（ifIndex 与
    //                             androidNetHandle 均为 0，且没能自动探测到）
    //                             时直接返回 BC_R_NO_PHYSICAL_INTERFACE，
    //                             不发出任何查询——绝不允许 socket 悄悄地
    //                             不绑定就把包发出去。
    //
    // ifIndex（非 Android）为 0 且 policy != OS 时会尝试用
    // tt_netmon_query_default_ifindex_ex 自动探测一次物理网卡（仅在定义了
    // TT_HAS_PATH_MONITOR 的构建里生效，与 SMPConnector 的既有用法一致）。
    //
    // host 本身是 IP 字面量时直接返回它，不查询。
    static DnsResult Resolve(const std::string& host, const DnsConfig& cfg);

    // 切网时调用（INetworkPathMonitor 回调里），清空缓存。
    static void ClearCache();

    // 默认公共 DNS，DnsConfig::servers 为空时使用。
    static const char* kDefaultServers[2];   // {"223.5.5.5", "8.8.8.8"}
};

#endif // TT_DNS_RESOLVER_H

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
