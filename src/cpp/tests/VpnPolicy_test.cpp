///////////////////////////////////////////////////////////////////////////////
// file : VpnPolicy_test.cpp
//
// Standalone unit test for the vpnPolicy resolution helpers. Not part of any
// build target; compile & run manually:
//
//   c++ -std=c++17 -Wall -Wextra -I src/cpp src/cpp/VpnPolicy.cpp \
//       src/cpp/tests/VpnPolicy_test.cpp -o /tmp/vpnpolicy_test && /tmp/vpnpolicy_test
//
// VpnPolicy.h 只 include NetworkRouteLookup.h（取 TT_ROUTE_IFINDEX_* 两个
// 哨兵常量），不需要链它的实现，所以依赖仍然是零。
//
// 覆盖：
//   1. vpnPolicy 字符串解析 / 回落矩阵（原有）；
//   2. tt_peer_address_class() 的地址段分类；
//   3. tt_vpn_pin_recheck() 的决策表 —— 含一个"改动前旧逻辑"的 oracle，
//      逐位断言 os / force-physical 两档以及"关掉地址段启发式"时的
//      prefer-physical 与改动前完全一致（这是本次修复的硬约束）。
//
///////////////////////////////////////////////////////////////////////////////
#include "VpnPolicy.h"

#include <cstdio>
#include <cstring>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

static void test_from_string()
{
    CHECK(tt_vpn_policy_from_string("os") == TT_VPN_POLICY_OS);
    CHECK(tt_vpn_policy_from_string("prefer-physical") == TT_VPN_POLICY_PREFER_PHYSICAL);
    CHECK(tt_vpn_policy_from_string("force-physical") == TT_VPN_POLICY_FORCE_PHYSICAL);
    // 未知 / 空 / NULL 一律 UNSET，由调用方决定是否告警
    CHECK(tt_vpn_policy_from_string("physical") == TT_VPN_POLICY_UNSET);
    CHECK(tt_vpn_policy_from_string("") == TT_VPN_POLICY_UNSET);
    CHECK(tt_vpn_policy_from_string(NULL) == TT_VPN_POLICY_UNSET);
    // 大小写敏感：不做归一化，避免和其它配置键的处理方式不一致
    CHECK(tt_vpn_policy_from_string("OS") == TT_VPN_POLICY_UNSET);
}

static void test_to_string()
{
    CHECK(strcmp(tt_vpn_policy_to_string(TT_VPN_POLICY_OS), "os") == 0);
    CHECK(strcmp(tt_vpn_policy_to_string(TT_VPN_POLICY_PREFER_PHYSICAL), "prefer-physical") == 0);
    CHECK(strcmp(tt_vpn_policy_to_string(TT_VPN_POLICY_FORCE_PHYSICAL), "force-physical") == 0);
    CHECK(strcmp(tt_vpn_policy_to_string(TT_VPN_POLICY_UNSET), "unset") == 0);
    // 越界值不能崩，也不能返回 NULL（日志里直接 %s 用）
    CHECK(tt_vpn_policy_to_string((TTVpnPolicy)99) != NULL);
}

// 完整的 4 x 3 组合矩阵：explicitPolicy x (hasBypassVpn, bypassVpn)
static void test_resolve_matrix()
{
    const TTVpnPolicy kDefault = TT_VPN_POLICY_PREFER_PHYSICAL;
    int both = -1;

    // --- explicitPolicy = UNSET：回落到 bypassVpn 兼容映射，再回落平台默认
    CHECK(tt_vpn_policy_resolve(TT_VPN_POLICY_UNSET, 0, 0, kDefault, &both) == kDefault);
    CHECK(both == 0);
    CHECK(tt_vpn_policy_resolve(TT_VPN_POLICY_UNSET, 1, 0, kDefault, &both) == TT_VPN_POLICY_OS);
    CHECK(both == 0);
    CHECK(tt_vpn_policy_resolve(TT_VPN_POLICY_UNSET, 1, 1, kDefault, &both) == TT_VPN_POLICY_PREFER_PHYSICAL);
    CHECK(both == 0);

    // --- explicitPolicy 已给：永远胜出，且同时给了 bypassVpn 时置 outBothGiven
    const TTVpnPolicy kExplicit[] = {
        TT_VPN_POLICY_OS, TT_VPN_POLICY_PREFER_PHYSICAL, TT_VPN_POLICY_FORCE_PHYSICAL,
    };
    for (int i = 0; i < 3; i++) {
        CHECK(tt_vpn_policy_resolve(kExplicit[i], 0, 0, kDefault, &both) == kExplicit[i]);
        CHECK(both == 0);
        CHECK(tt_vpn_policy_resolve(kExplicit[i], 1, 0, kDefault, &both) == kExplicit[i]);
        CHECK(both == 1);
        CHECK(tt_vpn_policy_resolve(kExplicit[i], 1, 1, kDefault, &both) == kExplicit[i]);
        CHECK(both == 1);
    }

    // 平台默认值是注入的，换一个也要正确回落（模拟 iOS）
    CHECK(tt_vpn_policy_resolve(TT_VPN_POLICY_UNSET, 0, 0, TT_VPN_POLICY_OS, &both) == TT_VPN_POLICY_OS);

    // outBothGiven 允许传 NULL
    CHECK(tt_vpn_policy_resolve(TT_VPN_POLICY_OS, 1, 1, kDefault, NULL) == TT_VPN_POLICY_OS);
}

// 平台默认值是编译期决定的，只能断言"当前宿主平台"的期望值
static void test_platform_default()
{
#if defined(__APPLE__) && TARGET_OS_IPHONE && !TARGET_OS_MACCATALYST
    CHECK(tt_vpn_policy_platform_default() == TT_VPN_POLICY_OS);
#else
    CHECK(tt_vpn_policy_platform_default() == TT_VPN_POLICY_PREFER_PHYSICAL);
#endif
}


///////////////////////////////////////////////////////////////////////////////
// tt_peer_address_class()
///////////////////////////////////////////////////////////////////////////////

static uint32_t ClassV4(const char* ip)
{
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    inet_pton(AF_INET, ip, &sin.sin_addr);
    return tt_peer_address_class((const struct sockaddr*)&sin, sizeof(sin));
}

static uint32_t ClassV6(const char* ip)
{
    struct sockaddr_in6 sin6;
    memset(&sin6, 0, sizeof(sin6));
    sin6.sin6_family = AF_INET6;
    inet_pton(AF_INET6, ip, &sin6.sin6_addr);
    return tt_peer_address_class((const struct sockaddr*)&sin6, sizeof(sin6));
}

static void test_peer_address_class()
{
    // --- 普通公网地址：GLOBAL（黑洞启发式不该碰它们）。
    //     34.117.59.81 是 ipinfo.io，本次端到端验证用的就是它。
    CHECK(ClassV4("34.117.59.81") == TT_PEER_CLASS_GLOBAL);
    CHECK(ClassV4("1.1.1.1")      == TT_PEER_CLASS_GLOBAL);
    CHECK(ClassV4("8.8.8.8")      == TT_PEER_CLASS_GLOBAL);
    CHECK(ClassV4("223.5.5.5")    == TT_PEER_CLASS_GLOBAL);
    CHECK(ClassV4("203.0.113.45")== TT_PEER_CLASS_GLOBAL);  // 公网示例地址
    CHECK(ClassV4("198.51.100.77")== TT_PEER_CLASS_GLOBAL); // 公网示例地址
    CHECK(ClassV6("2606:4700:4700::1111") == TT_PEER_CLASS_GLOBAL);

    // --- fake-IP 段：必须带 FAKE_IP 位，否则 Clash / Surge 的黑洞保护失效。
    //     198.18.0.0/15 同时是 RFC 2544 保留段，两个位都置。
    CHECK((ClassV4("198.18.0.0")   & TT_PEER_CLASS_FAKE_IP) != 0);
    CHECK((ClassV4("198.18.0.5")   & TT_PEER_CLASS_FAKE_IP) != 0);
    CHECK((ClassV4("198.19.255.255") & TT_PEER_CLASS_FAKE_IP) != 0);
    CHECK((ClassV4("198.18.0.5")   & TT_PEER_CLASS_PRIVATE) != 0);
    // 边界：198.17.x 与 198.20.x 不在 /15 内
    CHECK(ClassV4("198.17.255.255") == TT_PEER_CLASS_GLOBAL);
    CHECK(ClassV4("198.20.0.0")     == TT_PEER_CLASS_GLOBAL);
    // 28.0.0.0/8 是 Surge 旧默认。IANA 上是正常单播段，所以只置 FAKE_IP，
    // 不声称它"非全局可路由"。
    CHECK(ClassV4("28.0.0.1")   == TT_PEER_CLASS_FAKE_IP);
    CHECK(ClassV4("28.255.255.255") == TT_PEER_CLASS_FAKE_IP);
    CHECK(ClassV4("27.255.255.255") == TT_PEER_CLASS_GLOBAL);
    CHECK(ClassV4("29.0.0.0")       == TT_PEER_CLASS_GLOBAL);

    // --- 非全局可路由段。10.10.0.1 就是本机企业 VPN 的内网地址：改动后
    //     prefer-physical 必须仍然为它解绑，否则内网服务端连不上。
    CHECK((ClassV4("10.10.0.1")      & TT_PEER_CLASS_PRIVATE) != 0);
    CHECK((ClassV4("10.0.0.0")       & TT_PEER_CLASS_PRIVATE) != 0);
    CHECK((ClassV4("10.255.255.255") & TT_PEER_CLASS_PRIVATE) != 0);
    CHECK((ClassV4("172.16.0.1")     & TT_PEER_CLASS_PRIVATE) != 0);
    CHECK((ClassV4("172.31.255.255") & TT_PEER_CLASS_PRIVATE) != 0);
    CHECK((ClassV4("192.168.3.1")    & TT_PEER_CLASS_PRIVATE) != 0);
    CHECK((ClassV4("100.64.0.1")     & TT_PEER_CLASS_PRIVATE) != 0);
    CHECK((ClassV4("127.0.0.1")      & TT_PEER_CLASS_PRIVATE) != 0);
    CHECK((ClassV4("169.254.1.1")    & TT_PEER_CLASS_PRIVATE) != 0);
    CHECK((ClassV4("0.0.0.0")        & TT_PEER_CLASS_PRIVATE) != 0);
    CHECK((ClassV4("192.0.2.1")      & TT_PEER_CLASS_PRIVATE) != 0);
    CHECK((ClassV4("198.51.100.1")   & TT_PEER_CLASS_PRIVATE) != 0);
    CHECK((ClassV4("203.0.113.1")    & TT_PEER_CLASS_PRIVATE) != 0);
    CHECK((ClassV4("224.0.0.1")      & TT_PEER_CLASS_PRIVATE) != 0);
    CHECK((ClassV4("255.255.255.255")& TT_PEER_CLASS_PRIVATE) != 0);
    // 私网段的边界：紧邻的公网地址不能被误伤
    CHECK(ClassV4("9.255.255.255")   == TT_PEER_CLASS_GLOBAL);
    CHECK(ClassV4("11.0.0.0")        == TT_PEER_CLASS_GLOBAL);
    CHECK(ClassV4("172.15.255.255")  == TT_PEER_CLASS_GLOBAL);
    CHECK(ClassV4("172.32.0.0")      == TT_PEER_CLASS_GLOBAL);
    CHECK(ClassV4("192.167.255.255") == TT_PEER_CLASS_GLOBAL);
    CHECK(ClassV4("192.169.0.0")     == TT_PEER_CLASS_GLOBAL);
    CHECK(ClassV4("100.63.255.255")  == TT_PEER_CLASS_GLOBAL);
    CHECK(ClassV4("100.128.0.0")     == TT_PEER_CLASS_GLOBAL);

    // --- IPv6
    CHECK((ClassV6("fd00::1")   & TT_PEER_CLASS_PRIVATE) != 0);  // ULA
    CHECK((ClassV6("fc00::1")   & TT_PEER_CLASS_PRIVATE) != 0);
    CHECK((ClassV6("fe80::1")   & TT_PEER_CLASS_PRIVATE) != 0);  // 链路本地
    CHECK((ClassV6("ff02::1")   & TT_PEER_CLASS_PRIVATE) != 0);  // 组播
    CHECK((ClassV6("::1")       & TT_PEER_CLASS_PRIVATE) != 0);  // 环回
    CHECK((ClassV6("::")        & TT_PEER_CLASS_PRIVATE) != 0);  // 未指定
    CHECK((ClassV6("2001:db8::1")& TT_PEER_CLASS_PRIVATE) != 0); // 文档段
    CHECK(ClassV6("2001:4860:4860::8888") == TT_PEER_CLASS_GLOBAL);
    // IPv4-mapped 按内嵌 v4 分类，否则整整一类会漏判
    CHECK((ClassV6("::ffff:10.10.0.1")   & TT_PEER_CLASS_PRIVATE) != 0);
    CHECK((ClassV6("::ffff:198.18.0.5")  & TT_PEER_CLASS_FAKE_IP) != 0);
    CHECK(ClassV6("::ffff:34.117.59.81") == TT_PEER_CLASS_GLOBAL);

    // --- 退化输入一律 GLOBAL（"不知道"不该凭空制造黑洞判定）
    CHECK(tt_peer_address_class(NULL, 0) == TT_PEER_CLASS_GLOBAL);
    struct sockaddr_in shortSin;
    memset(&shortSin, 0, sizeof(shortSin));
    shortSin.sin_family = AF_INET;
    inet_pton(AF_INET, "10.10.0.1", &shortSin.sin_addr);
    // 长度不足：不能越界读，也不能把它判成私网
    CHECK(tt_peer_address_class((const struct sockaddr*)&shortSin, 4)
          == TT_PEER_CLASS_GLOBAL);
    struct sockaddr_un_like { unsigned short fam; char path[16]; } other;
    memset(&other, 0, sizeof(other));
    other.fam = AF_UNIX;
    CHECK(tt_peer_address_class((const struct sockaddr*)&other, sizeof(other))
          == TT_PEER_CLASS_GLOBAL);
}

///////////////////////////////////////////////////////////////////////////////
// tt_vpn_pin_recheck()
///////////////////////////////////////////////////////////////////////////////

// 改动**前**的判定逻辑，逐字照搬自旧 UDPSender::Connect()：
//
//     natural_idx = tt_route_lookup_ifindex(peer, len, /*scope=*/0);
//     mismatch         = (natural_idx != 0 && natural_idx != bound);
//     fake_ip_fallback = (natural_idx == 0 && _IsFakeIPPeer(peer));   // 仅 Apple
//     if (mismatch || fake_ip_fallback) {
//         if (force-physical) { 只打日志，不解绑 }
//         else                { 解绑 }
//     }
//
// 用它当 oracle，把"哪几档必须逐位不变"变成可执行断言。
static TTPinVerdict OldVerdict(TTVpnPolicy policy,
                               uint32_t bound,
                               uint32_t global_idx,
                               int isFakeIP)
{
    if (bound == 0) {
        // 旧代码里 bound==0 意味着 m_bInterfaceBindingActive 为假，整块被跳过
        return TT_PIN_VERDICT_KEEP;
    }
    if (policy == TT_VPN_POLICY_FORCE_PHYSICAL) {
        return TT_PIN_VERDICT_KEEP;
    }
    const int mismatch = (global_idx != 0 && global_idx != bound);
    const int fakeFallback = (global_idx == 0 && isFakeIP);
    return (mismatch || fakeFallback) ? TT_PIN_VERDICT_UNPIN : TT_PIN_VERDICT_KEEP;
}

static TTPinVerdict New(TTVpnPolicy policy, uint32_t bound,
                        uint32_t scoped, uint32_t global_idx, uint32_t cls)
{
    return tt_vpn_pin_recheck(policy, bound, scoped, global_idx, cls, NULL);
}

// 本机实测拓扑（macOS 15.6.1 + GlobalProtect 全隧道）：
//   en0   = ifIndex 14（物理网卡，有默认路由 192.168.3.1）
//   utun4 = ifIndex 22（VPN，抢占全局默认路由 10.10.0.71）
#define IF_PHY  14u
#define IF_VPN  22u
#define R_UNK   TT_ROUTE_IFINDEX_UNKNOWN
#define R_NOR   TT_ROUTE_IFINDEX_NO_ROUTE

// 这是本次修复的核心用例。实测路由表答案：
//   route -n get            34.117.59.81  -> utun4  (=22)
//   route -n get -ifscope en0 34.117.59.81 -> en0    (=14)
static void test_recheck_the_bug_being_fixed()
{
    TTPinRecheckReason why = TT_PIN_REASON_SCOPED_MISMATCH;

    // prefer-physical：改动后必须**保持绑定**——这就是 QUIC 与 TCP 行为对齐。
    CHECK(tt_vpn_pin_recheck(TT_VPN_POLICY_PREFER_PHYSICAL, IF_PHY,
                             /*scoped=*/IF_PHY, /*global=*/IF_VPN,
                             TT_PEER_CLASS_GLOBAL, &why)
          == TT_PIN_VERDICT_KEEP);
    CHECK(why == TT_PIN_REASON_OK);
    // 同一组输入，改动前是解绑（回落 VPN，服务端看到 VPN 出口 IP）。
    // 这一行断言的就是"行为确实变了"，防止修复被无声回退。
    CHECK(OldVerdict(TT_VPN_POLICY_PREFER_PHYSICAL, IF_PHY, IF_VPN, 0)
          == TT_PIN_VERDICT_UNPIN);

    // force-physical：本来就不解绑，前后一致。
    CHECK(New(TT_VPN_POLICY_FORCE_PHYSICAL, IF_PHY, IF_PHY, IF_VPN,
              TT_PEER_CLASS_GLOBAL) == TT_PIN_VERDICT_KEEP);

    // os：语义是跟随系统，全局表说走 VPN 就跟着走 —— 逐位保持旧行为。
    CHECK(tt_vpn_pin_recheck(TT_VPN_POLICY_OS, IF_PHY, IF_PHY, IF_VPN,
                             TT_PEER_CLASS_GLOBAL, &why)
          == TT_PIN_VERDICT_UNPIN);
    CHECK(why == TT_PIN_REASON_GLOBAL_MISMATCH);
}

// fake-IP / 内网地址的黑洞保护：**一点不能减**。
// 实测（本机，无 Clash）：
//   route -n get            198.18.0.5   -> utun4
//   route -n get -ifscope en0 198.18.0.5  -> en0     <- scoped 查询答不出问题
//   route -n get            10.10.0.1    -> utun4
//   route -n get -ifscope en0 10.10.0.1   -> en0     <- 同上
// 所以只要绑定网卡有默认路由，scoped 判据对这两类地址恒为"一致"。黑洞判定
// 必须靠地址段 + 全局表，这组用例就是钉死这一点。
static void test_recheck_blackhole_protection_not_weakened()
{
    TTPinRecheckReason why = TT_PIN_REASON_OK;
    const uint32_t kFake    = TT_PEER_CLASS_FAKE_IP | TT_PEER_CLASS_PRIVATE;
    const uint32_t kPrivate = TT_PEER_CLASS_PRIVATE;

    // fake-IP，scoped 说"一致"（macOS 的真实行为），全局说走 VPN -> 必须解绑
    CHECK(tt_vpn_pin_recheck(TT_VPN_POLICY_PREFER_PHYSICAL, IF_PHY,
                             /*scoped=*/IF_PHY, /*global=*/IF_VPN,
                             kFake, &why) == TT_PIN_VERDICT_UNPIN);
    CHECK(why == TT_PIN_REASON_BLACKHOLE_RISK);

    // fake-IP，全局查询失败（沙箱 / EPERM）-> 仍必须解绑（旧代码的兜底路径）
    CHECK(tt_vpn_pin_recheck(TT_VPN_POLICY_PREFER_PHYSICAL, IF_PHY,
                             IF_PHY, R_UNK, kFake, &why)
          == TT_PIN_VERDICT_UNPIN);
    CHECK(why == TT_PIN_REASON_BLACKHOLE_RISK);

    // 内网地址（企业 VPN 后面的服务端）-> 必须解绑，否则内网连不上。
    // 这条是修复过程中新发现的：只把 mismatch 改成 scoped 会让它退化成
    // "保持绑定 + 静默超时"。
    CHECK(tt_vpn_pin_recheck(TT_VPN_POLICY_PREFER_PHYSICAL, IF_PHY,
                             IF_PHY, IF_VPN, kPrivate, &why)
          == TT_PIN_VERDICT_UNPIN);
    CHECK(why == TT_PIN_REASON_BLACKHOLE_RISK);

    // 非全局可路由，但全局表也说就该走绑定的这块网卡（例如同一网段的
    // 192.168.3.1，本机实测 global 与 scoped 都是 en0）-> 不该解绑。
    CHECK(New(TT_VPN_POLICY_PREFER_PHYSICAL, IF_PHY, IF_PHY, IF_PHY, kPrivate)
          == TT_PIN_VERDICT_KEEP);

    // 穷举：对**所有** peerClass != GLOBAL 的输入，新逻辑必须是旧逻辑的超集
    // （旧解绑的，新也必须解绑）。这是"保护不减"的可执行证明。
    const uint32_t kClasses[] = { TT_PEER_CLASS_FAKE_IP, TT_PEER_CLASS_PRIVATE,
                                  TT_PEER_CLASS_FAKE_IP | TT_PEER_CLASS_PRIVATE };
    const uint32_t kIdx[] = { R_UNK, IF_PHY, IF_VPN, 99u, R_NOR };
    for (size_t c = 0; c < sizeof(kClasses)/sizeof(kClasses[0]); ++c) {
        for (size_t g = 0; g < sizeof(kIdx)/sizeof(kIdx[0]); ++g) {
            for (size_t sc = 0; sc < sizeof(kIdx)/sizeof(kIdx[0]); ++sc) {
                const int isFake =
                    (kClasses[c] & TT_PEER_CLASS_FAKE_IP) != 0 ? 1 : 0;
                if (OldVerdict(TT_VPN_POLICY_PREFER_PHYSICAL, IF_PHY,
                               kIdx[g], isFake) == TT_PIN_VERDICT_UNPIN) {
                    CHECK(New(TT_VPN_POLICY_PREFER_PHYSICAL, IF_PHY,
                              kIdx[sc], kIdx[g], kClasses[c])
                          == TT_PIN_VERDICT_UNPIN);
                }
            }
        }
    }
}

// prefer-physical 的绑定有效性判据（scoped）单独成组。
static void test_recheck_binding_validity()
{
    TTPinRecheckReason why = TT_PIN_REASON_OK;

    // scoped 说"绑了也走别人" -> 解绑
    CHECK(tt_vpn_pin_recheck(TT_VPN_POLICY_PREFER_PHYSICAL, IF_PHY,
                             /*scoped=*/IF_VPN, /*global=*/IF_VPN,
                             TT_PEER_CLASS_GLOBAL, &why)
          == TT_PIN_VERDICT_UNPIN);
    CHECK(why == TT_PIN_REASON_SCOPED_MISMATCH);

    // scoped 说"这块网卡根本没有到对端的路由" -> 确定性不可达，解绑。
    // NO_ROUTE 必须与 UNKNOWN 区分：前者是答案，后者是"没答案"。
    CHECK(tt_vpn_pin_recheck(TT_VPN_POLICY_PREFER_PHYSICAL, IF_PHY,
                             R_NOR, IF_VPN, TT_PEER_CLASS_GLOBAL, &why)
          == TT_PIN_VERDICT_UNPIN);
    CHECK(why == TT_PIN_REASON_SCOPED_NO_ROUTE);

    // scoped 查不出来（iOS / Android 桩，或沙箱）+ 普通公网地址 -> 保持绑定。
    // "不知道"不能当失败信号，这是 NetworkRouteLookup.h 明确的契约。
    CHECK(tt_vpn_pin_recheck(TT_VPN_POLICY_PREFER_PHYSICAL, IF_PHY,
                             R_UNK, R_UNK, TT_PEER_CLASS_GLOBAL, &why)
          == TT_PIN_VERDICT_KEEP);
    CHECK(why == TT_PIN_REASON_OK);
    CHECK(New(TT_VPN_POLICY_PREFER_PHYSICAL, IF_PHY, R_UNK, IF_VPN,
              TT_PEER_CLASS_GLOBAL) == TT_PIN_VERDICT_KEEP);

    // 没绑定就无从复核，任何输入都 KEEP
    const uint32_t kIdx[] = { R_UNK, IF_PHY, IF_VPN, R_NOR };
    for (size_t i = 0; i < sizeof(kIdx)/sizeof(kIdx[0]); ++i) {
        for (size_t j = 0; j < sizeof(kIdx)/sizeof(kIdx[0]); ++j) {
            CHECK(tt_vpn_pin_recheck(TT_VPN_POLICY_PREFER_PHYSICAL, 0,
                                     kIdx[i], kIdx[j],
                                     TT_PEER_CLASS_FAKE_IP, &why)
                  == TT_PIN_VERDICT_KEEP);
            CHECK(why == TT_PIN_REASON_OK);
        }
    }
}

// 硬约束的可执行版本：os / force-physical 两档，以及"关掉地址段启发式"
// （非 Apple 平台的实际调用形态）时的 prefer-physical，必须与改动前**逐位**
// 一致。穷举 scoped x global x class x policy 全组合。
static void test_recheck_matches_old_behaviour_where_required()
{
    const uint32_t kIdx[] = { R_UNK, IF_PHY, IF_VPN, 99u, R_NOR };
    const uint32_t kClasses[] = {
        TT_PEER_CLASS_GLOBAL,
        TT_PEER_CLASS_FAKE_IP,
        TT_PEER_CLASS_PRIVATE,
        TT_PEER_CLASS_FAKE_IP | TT_PEER_CLASS_PRIVATE,
    };
    const uint32_t kBound[] = { 0u, IF_PHY, IF_VPN };
    const TTVpnPolicy kUnchanged[] = {
        TT_VPN_POLICY_OS, TT_VPN_POLICY_FORCE_PHYSICAL, TT_VPN_POLICY_UNSET,
    };

    for (size_t b = 0; b < sizeof(kBound)/sizeof(kBound[0]); ++b) {
      for (size_t sc = 0; sc < sizeof(kIdx)/sizeof(kIdx[0]); ++sc) {
        for (size_t g = 0; g < sizeof(kIdx)/sizeof(kIdx[0]); ++g) {
          for (size_t c = 0; c < sizeof(kClasses)/sizeof(kClasses[0]); ++c) {
            const int isFake = (kClasses[c] & TT_PEER_CLASS_FAKE_IP) != 0 ? 1 : 0;

            // os / force-physical / unset：三档与改动前逐位一致，**只允许一处
            // 刻意的放宽** —— 见下面的 kIntendedWidening。
            //
            // 放宽的那一格是：全局查询查不出来（UNKNOWN）且对端是"非全局可
            // 路由但不在 fake-IP 段"（例如 10.x / 127.x）。旧逻辑不解绑，新
            // 逻辑解绑。对 os 档来说解绑永远是安全的（该档语义就是跟随系统
            // 路由），所以这是纯收益。这里不是把断言放松成"随便"，而是把
            // 允许的偏差**精确到一格**，其余全组合仍然要求逐位一致。
            const int kIntendedWidening =
                (kBound[b] != 0) &&
                (kIdx[g] == R_UNK) &&
                (kClasses[c] != TT_PEER_CLASS_GLOBAL) &&
                ((kClasses[c] & TT_PEER_CLASS_FAKE_IP) == 0);

            for (size_t p = 0; p < sizeof(kUnchanged)/sizeof(kUnchanged[0]); ++p) {
                const TTPinVerdict now =
                    New(kUnchanged[p], kBound[b], kIdx[sc], kIdx[g], kClasses[c]);
                const TTPinVerdict before =
                    OldVerdict(kUnchanged[p], kBound[b], kIdx[g], isFake);

                if (kIntendedWidening &&
                    kUnchanged[p] != TT_VPN_POLICY_FORCE_PHYSICAL) {
                    // 放宽只作用在会解绑的那两档；force-physical 恒 KEEP，
                    // 不受影响，所以它在任何一格都必须逐位一致。
                    CHECK(before == TT_PIN_VERDICT_KEEP);
                    CHECK(now    == TT_PIN_VERDICT_UNPIN);
                } else {
                    CHECK(now == before);
                }
            }

            // prefer-physical + peerClass=GLOBAL 就是 Linux / Windows 上的实际
            // 调用形态（那两个平台不启用地址段启发式）。而且它们的
            // tt_route_lookup_ifindex 刻意忽略 scope，所以 scoped == global。
            // 在这个约束下也必须与改动前逐位一致。
            CHECK(New(TT_VPN_POLICY_PREFER_PHYSICAL, kBound[b],
                      /*scoped=*/kIdx[g], /*global=*/kIdx[g],
                      TT_PEER_CLASS_GLOBAL)
                  == OldVerdict(TT_VPN_POLICY_PREFER_PHYSICAL, kBound[b],
                                kIdx[g], /*isFakeIP=*/0));
          }
        }
      }
    }
}


// concern 4 的放宽：os 档在"全局查询查不出来 + 对端非全局可路由"时也解绑。
// 旧逻辑只对 fake-IP 两段兜底，那个收窄纯粹是为了满足"逐位不变"的约束。
static void test_os_widened_blackhole_fallback()
{
    TTPinRecheckReason why = TT_PIN_REASON_OK;
    const uint32_t kPrivate = TT_PEER_CLASS_PRIVATE;   // 例如 10.10.0.1 / 127.0.0.1

    // 全局查询失败 + 私网对端 -> 解绑（新行为）
    CHECK(tt_vpn_pin_recheck(TT_VPN_POLICY_OS, IF_PHY, IF_PHY, R_UNK,
                             kPrivate, &why) == TT_PIN_VERDICT_UNPIN);
    CHECK(why == TT_PIN_REASON_BLACKHOLE_RISK);
    // 旧逻辑在同一组输入下不解绑 —— 钉住"这里确实放宽了"
    CHECK(OldVerdict(TT_VPN_POLICY_OS, IF_PHY, R_UNK, /*isFakeIP=*/0)
          == TT_PIN_VERDICT_KEEP);

    // fake-IP 仍然解绑（旧行为，未变）
    CHECK(New(TT_VPN_POLICY_OS, IF_PHY, IF_PHY, R_UNK,
              TT_PEER_CLASS_FAKE_IP) == TT_PIN_VERDICT_UNPIN);

    // 普通公网地址 + 全局查询失败 -> 仍然保持绑定（不能因为放宽就乱解绑）
    CHECK(New(TT_VPN_POLICY_OS, IF_PHY, IF_PHY, R_UNK,
              TT_PEER_CLASS_GLOBAL) == TT_PIN_VERDICT_KEEP);

    // force-physical 不受放宽影响，恒 KEEP
    CHECK(New(TT_VPN_POLICY_FORCE_PHYSICAL, IF_PHY, IF_PHY, R_UNK,
              kPrivate) == TT_PIN_VERDICT_KEEP);
}

static void test_pin_reason_to_string()
{
    CHECK(strcmp(tt_vpn_pin_reason_to_string(TT_PIN_REASON_OK), "ok") == 0);
    CHECK(strcmp(tt_vpn_pin_reason_to_string(TT_PIN_REASON_SCOPED_MISMATCH),
                 "scoped-mismatch") == 0);
    CHECK(strcmp(tt_vpn_pin_reason_to_string(TT_PIN_REASON_SCOPED_NO_ROUTE),
                 "scoped-no-route") == 0);
    CHECK(strcmp(tt_vpn_pin_reason_to_string(TT_PIN_REASON_BLACKHOLE_RISK),
                 "blackhole-risk") == 0);
    CHECK(strcmp(tt_vpn_pin_reason_to_string(TT_PIN_REASON_GLOBAL_MISMATCH),
                 "global-mismatch") == 0);
    // 越界值不能返回 NULL（日志里直接 %s 用）
    CHECK(tt_vpn_pin_reason_to_string((TTPinRecheckReason)99) != NULL);
}

int main()
{
    test_from_string();
    test_to_string();
    test_resolve_matrix();
    test_platform_default();
    test_peer_address_class();
    test_recheck_the_bug_being_fixed();
    test_recheck_blackhole_protection_not_weakened();
    test_recheck_binding_validity();
    test_recheck_matches_old_behaviour_where_required();
    test_os_widened_blackhole_fallback();
    test_pin_reason_to_string();
    if (g_failures == 0) {
        printf("VpnPolicy_test: ALL PASS\n");
        return 0;
    }
    printf("VpnPolicy_test: %d FAILURE(S)\n", g_failures);
    return 1;
}
