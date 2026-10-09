///////////////////////////////////////////////////////////////////////////////
// file   : VpnPolicy.cpp
// author : anto
///////////////////////////////////////////////////////////////////////////////

#include "VpnPolicy.h"

#include <string.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#endif

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

extern "C" {

TTVpnPolicy tt_vpn_policy_platform_default(void)
{
    // Mac Catalyst 虽然 TARGET_OS_IPHONE=1，但跑在桌面上、面对的是同一批
    // TUN 模式代理，所以按桌面处理。
#if defined(__APPLE__) && TARGET_OS_IPHONE && !TARGET_OS_MACCATALYST
    return TT_VPN_POLICY_OS;
#else
    return TT_VPN_POLICY_PREFER_PHYSICAL;
#endif
}

TTVpnPolicy tt_vpn_policy_from_string(const char* s)
{
    if (s == NULL || s[0] == '\0') {
        return TT_VPN_POLICY_UNSET;
    }
    if (strcmp(s, "os") == 0) {
        return TT_VPN_POLICY_OS;
    }
    if (strcmp(s, "prefer-physical") == 0) {
        return TT_VPN_POLICY_PREFER_PHYSICAL;
    }
    if (strcmp(s, "force-physical") == 0) {
        return TT_VPN_POLICY_FORCE_PHYSICAL;
    }
    return TT_VPN_POLICY_UNSET;
}

const char* tt_vpn_policy_to_string(TTVpnPolicy p)
{
    switch (p) {
        case TT_VPN_POLICY_OS:              return "os";
        case TT_VPN_POLICY_PREFER_PHYSICAL: return "prefer-physical";
        case TT_VPN_POLICY_FORCE_PHYSICAL:  return "force-physical";
        case TT_VPN_POLICY_UNSET:           return "unset";
        default:                            return "invalid";
    }
}

TTVpnPolicy tt_vpn_policy_resolve(TTVpnPolicy explicitPolicy,
                                  int hasBypassVpn,
                                  int bypassVpn,
                                  TTVpnPolicy platformDefault,
                                  int* outBothGiven)
{
    const int explicitGiven = (explicitPolicy == TT_VPN_POLICY_OS ||
                               explicitPolicy == TT_VPN_POLICY_PREFER_PHYSICAL ||
                               explicitPolicy == TT_VPN_POLICY_FORCE_PHYSICAL);

    if (outBothGiven) {
        *outBothGiven = (explicitGiven && hasBypassVpn) ? 1 : 0;
    }
    if (explicitGiven) {
        return explicitPolicy;
    }
    if (hasBypassVpn) {
        return bypassVpn ? TT_VPN_POLICY_PREFER_PHYSICAL : TT_VPN_POLICY_OS;
    }
    return platformDefault;
}

///////////////////////////////////////////////////////////////////////////////
// 对端地址分类
///////////////////////////////////////////////////////////////////////////////

// IPv4 分类。ip 是**主机字节序**。
static uint32_t _classify_v4(uint32_t ip)
{
    uint32_t cls = TT_PEER_CLASS_GLOBAL;

    // fake-IP 段先判。198.18.0.0/15 同时也是非全局可路由（RFC 2544 保留），
    // 两个位都置上；28.0.0.0/8 在 IANA 是已分配的正常单播段，只有 Surge 把它
    // 挪用成 fake-IP，所以只置 FAKE_IP 位，不声称它"非全局可路由"。
    if ((ip & 0xFFFE0000u) == 0xC6120000u)  // 198.18.0.0/15
    {
        cls |= TT_PEER_CLASS_FAKE_IP | TT_PEER_CLASS_PRIVATE;
        return cls;
    }
    if ((ip & 0xFF000000u) == 0x1C000000u)  // 28.0.0.0/8
    {
        cls |= TT_PEER_CLASS_FAKE_IP;
        return cls;
    }

    // 非全局可路由段（RFC 6890 特殊用途地址里，会被当作 QUIC/TCP 对端出现的
    // 那些）。列表刻意保守：只收"从物理网卡硬发出去必然到不了"的段。
    if ((ip & 0xFF000000u) == 0x00000000u ||  // 0.0.0.0/8      本网络
        (ip & 0xFF000000u) == 0x0A000000u ||  // 10.0.0.0/8     私网
        (ip & 0xFFC00000u) == 0x64400000u ||  // 100.64.0.0/10  CGNAT
        (ip & 0xFF000000u) == 0x7F000000u ||  // 127.0.0.0/8    环回
        (ip & 0xFFFF0000u) == 0xA9FE0000u ||  // 169.254.0.0/16 链路本地
        (ip & 0xFFF00000u) == 0xAC100000u ||  // 172.16.0.0/12  私网
        (ip & 0xFFFFFF00u) == 0xC0000000u ||  // 192.0.0.0/24   IETF 协议分配
        (ip & 0xFFFFFF00u) == 0xC0000200u ||  // 192.0.2.0/24   TEST-NET-1
        (ip & 0xFFFF0000u) == 0xC0A80000u ||  // 192.168.0.0/16 私网
        (ip & 0xFFFFFF00u) == 0xC6336400u ||  // 198.51.100.0/24 TEST-NET-2
        (ip & 0xFFFFFF00u) == 0xCB007100u ||  // 203.0.113.0/24 TEST-NET-3
        (ip & 0xF0000000u) == 0xE0000000u ||  // 224.0.0.0/4    组播
        (ip & 0xF0000000u) == 0xF0000000u)    // 240.0.0.0/4    保留 + 广播
    {
        cls |= TT_PEER_CLASS_PRIVATE;
    }
    return cls;
}

uint32_t tt_peer_address_class(const struct sockaddr* dst, size_t dst_len)
{
    if (dst == NULL)
    {
        return TT_PEER_CLASS_GLOBAL;
    }

    if (dst->sa_family == AF_INET)
    {
        if (dst_len < sizeof(struct sockaddr_in))
        {
            return TT_PEER_CLASS_GLOBAL;
        }
        const struct sockaddr_in* sin = (const struct sockaddr_in*)dst;
        return _classify_v4(ntohl((uint32_t)sin->sin_addr.s_addr));
    }

    if (dst->sa_family == AF_INET6)
    {
        if (dst_len < sizeof(struct sockaddr_in6))
        {
            return TT_PEER_CLASS_GLOBAL;
        }
        const struct sockaddr_in6* sin6 = (const struct sockaddr_in6*)dst;
        const unsigned char* a = (const unsigned char*)&sin6->sin6_addr;

        // IPv4-mapped（::ffff:a.b.c.d）与 IPv4-compatible（::a.b.c.d）都按内嵌
        // 的 IPv4 分类——DNS / 双栈栈内转换都可能把 v4 对端交上来成这个形态，
        // 漏掉它就等于漏掉那一整类。
        static const unsigned char kV4MappedPrefix[12] = {
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF
        };
        if (memcmp(a, kV4MappedPrefix, 12) == 0)
        {
            uint32_t v4 = ((uint32_t)a[12] << 24) | ((uint32_t)a[13] << 16) |
                          ((uint32_t)a[14] << 8)  |  (uint32_t)a[15];
            return _classify_v4(v4);
        }

        uint32_t cls = TT_PEER_CLASS_GLOBAL;
        if ((a[0] & 0xFE) == 0xFC)                       // fc00::/7   ULA
        {
            cls |= TT_PEER_CLASS_PRIVATE;
        }
        else if (a[0] == 0xFE && (a[1] & 0xC0) == 0x80)  // fe80::/10  链路本地
        {
            cls |= TT_PEER_CLASS_PRIVATE;
        }
        else if (a[0] == 0xFF)                           // ff00::/8   组播
        {
            cls |= TT_PEER_CLASS_PRIVATE;
        }
        else if (a[0] == 0x20 && a[1] == 0x01 &&
                 a[2] == 0x0D && a[3] == 0xB8)           // 2001:db8::/32 文档
        {
            cls |= TT_PEER_CLASS_PRIVATE;
        }
        else
        {
            // ::/128 未指定 与 ::1/128 环回
            int allZeroHigh = 1;
            for (int i = 0; i < 15; ++i)
            {
                if (a[i] != 0) { allZeroHigh = 0; break; }
            }
            if (allZeroHigh && (a[15] == 0 || a[15] == 1))
            {
                cls |= TT_PEER_CLASS_PRIVATE;
            }
        }
        return cls;
    }

    return TT_PEER_CLASS_GLOBAL;
}

///////////////////////////////////////////////////////////////////////////////
// 绑定复核决策
///////////////////////////////////////////////////////////////////////////////

const char* tt_vpn_pin_reason_to_string(TTPinRecheckReason r)
{
    switch (r) {
        case TT_PIN_REASON_OK:              return "ok";
        case TT_PIN_REASON_SCOPED_MISMATCH: return "scoped-mismatch";
        case TT_PIN_REASON_SCOPED_NO_ROUTE: return "scoped-no-route";
        case TT_PIN_REASON_BLACKHOLE_RISK:  return "blackhole-risk";
        case TT_PIN_REASON_GLOBAL_MISMATCH: return "global-mismatch";
        default:                            return "invalid";
    }
}

TTPinVerdict tt_vpn_pin_recheck(TTVpnPolicy policy,
                                uint32_t boundIfIndex,
                                uint32_t scopedIfIndex,
                                uint32_t globalIfIndex,
                                uint32_t peerClass,
                                TTPinRecheckReason* outReason)
{
    TTPinRecheckReason reason = TT_PIN_REASON_OK;
    TTPinVerdict verdict = TT_PIN_VERDICT_KEEP;

    // 没钉住就无从复核。也顺手挡掉 boundIfIndex==0 时"0 != 0 为假"之类的巧合。
    if (boundIfIndex == 0)
    {
        goto done;
    }

    // force-physical 永不解绑。诊断日志由调用方打（它才拿得到 IP 字符串与
    // logger 上下文）。
    if (policy == TT_VPN_POLICY_FORCE_PHYSICAL)
    {
        goto done;
    }

    if (policy == TT_VPN_POLICY_PREFER_PHYSICAL)
    {
        // (1) 绑定有效性：问 scoped 路由表。
        //     NO_ROUTE 是**确定性**的否定答案（被绑网卡的 scoped 表里没有到
        //     对端的路由），不是"不知道"，必须和 UNKNOWN 分开处理。
        if (scopedIfIndex == TT_ROUTE_IFINDEX_NO_ROUTE)
        {
            reason  = TT_PIN_REASON_SCOPED_NO_ROUTE;
            verdict = TT_PIN_VERDICT_UNPIN;
            goto done;
        }
        if (scopedIfIndex != TT_ROUTE_IFINDEX_UNKNOWN &&
            scopedIfIndex != boundIfIndex)
        {
            reason  = TT_PIN_REASON_SCOPED_MISMATCH;
            verdict = TT_PIN_VERDICT_UNPIN;
            goto done;
        }

        // (2) 黑洞保护：地址段 + 全局路由表。见头文件里的长注释——scoped 查询
        //     结构上答不了这个问题，所以它必须是独立的一条判据。
        //     "全局表查不出来（UNKNOWN）"也算命中：这正是旧代码
        //     natural_idx == 0 那条 fake-IP 兜底覆盖的情形。
        if (peerClass != TT_PEER_CLASS_GLOBAL &&
            (globalIfIndex == TT_ROUTE_IFINDEX_UNKNOWN ||
             globalIfIndex != boundIfIndex))
        {
            reason  = TT_PIN_REASON_BLACKHOLE_RISK;
            verdict = TT_PIN_VERDICT_UNPIN;
            goto done;
        }
        goto done;
    }

    // os / unset：语义是"跟随系统路由"，全局表说走别处就跟着走。
    // ⚠️ 这一段是逐位保持的既有已上线行为，不要改成 scoped。
    if (globalIfIndex != TT_ROUTE_IFINDEX_UNKNOWN &&
        globalIfIndex != boundIfIndex)
    {
        reason  = TT_PIN_REASON_GLOBAL_MISMATCH;
        verdict = TT_PIN_VERDICT_UNPIN;
        goto done;
    }
    // 全局查询查不出来（沙箱 / EPERM）时的黑洞兜底。
    //
    // 这里看的是整个 peerClass，不只是 FAKE_IP 位。上一版刻意只看 FAKE_IP，
    // 纯粹为了让"os 档与改动前逐位一致"这条约束成立；但那个收窄本身没有道理：
    // os 档的语义就是"跟随系统路由"，因此对这一档来说**解绑永远是安全的**——
    // 放弃我们自己加的绑定只会让流量回到内核本来的选择。既然如此，把环回 /
    // 私网 / CGNAT 等同样"从物理网卡发出去必然黑洞"的地址一起纳入，比只保
    // fake-IP 两段更自洽，也和 prefer-physical 那一支的判据对齐。
    if (globalIfIndex == TT_ROUTE_IFINDEX_UNKNOWN &&
        peerClass != TT_PEER_CLASS_GLOBAL)
    {
        reason  = TT_PIN_REASON_BLACKHOLE_RISK;
        verdict = TT_PIN_VERDICT_UNPIN;
        goto done;
    }

done:
    if (outReason != NULL)
    {
        *outReason = reason;
    }
    return verdict;
}

} // extern "C"

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
