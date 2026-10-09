///////////////////////////////////////////////////////////////////////////////
// file : TcpChannel_test.cpp
//
// Standalone unit test for TcpChannel. Not part of any build target;
// compile & run manually:
//
//   c++ -std=c++17 -I src/cpp -I deps/env/src -I deps/jquic/include \
//       -I deps/boringssl/src/include \
//       src/cpp/VpnPolicy.cpp src/cpp/SocketPinner.cpp src/cpp/DnsResolver.cpp \
//       src/cpp/TlsContext.cpp src/cpp/SSLayer.cpp src/cpp/Runtime.cpp \
//       src/cpp/NetworkRouteLookup.cpp src/cpp/apple/AppleRouteLookup.cpp \
//       src/cpp/TcpChannel.cpp \
//       src/cpp/Utils.cpp deps/env/src/BC/BCSockAddr.cpp \
//       src/cpp/tests/TcpChannel_test.cpp \
//       deps/env/lib/Darwin/arm64/Debug/libenv.a \
//       deps/boringssl/lib/Darwin/arm64/Debug/libssl.a \
//       deps/boringssl/lib/Darwin/arm64/Debug/libcrypto.a \
//       -o /tmp/tcpchannel_test && /tmp/tcpchannel_test
//
// 依赖比 DnsResolver_test 更长一截，原因如下：
//   * TcpChannel.cpp 要 DnsResolver / SocketPinner / TlsContext / SSLayer /
//     NetworkRouteLookup / Runtime 的实现（不是桩）；
//   * NetworkRouteLookup.cpp 在 Apple 上编译成空 TU，tt_route_lookup_ifindex
//     的真身在 apple/AppleRouteLookup.cpp 里，两个都要给；Linux 换成
//     src/cpp/linux/LinuxRouteLookup.cpp，Windows 换成
//     src/cpp/win32/WinRouteLookup.cpp；
//   * TlsContext / SSLayer 要 boringssl 的 libssl + libcrypto；
//   * 走 StdAfx.h/Utils.h 的真实 LogQ，所以要 Utils.cpp + libenv.a，
//     -I deps/jquic/include 是因为 Utils.h 传递性 include 了 <xquic/xquic.h>。
// 非 Darwin/arm64 宿主换成对应平台下的 deps/*/lib/<平台>/<架构>/Debug/*.a。
//
// 本测试**不发任何网络请求、不初始化 Runtime**，只覆盖三块不依赖事件循环的
// 逻辑：状态机迁移表、IP 字面量解析、Open() 的同步参数与策略校验。
//
// ⚠️ 这里刻意不定义 TT_HAS_PATH_MONITOR —— 这正是 Task 4 栽过的那个坑的
//    回归测试场景：guard 关闭时 force-physical 必须硬失败，绝不能静默放行。
///////////////////////////////////////////////////////////////////////////////
#include "TcpChannel.h"

#include <cstdio>
#include <cstring>
#include <string>

static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

///////////////////////////////////////////////////////////////////////////////
// 状态机迁移表
///////////////////////////////////////////////////////////////////////////////

static void test_happy_path_transitions()
{
    // TLS 路径：IDLE → RESOLVING → PINNING → CONNECTING → TLS_HANDSHAKE → READY
    CHECK(TcpChannel::IsLegalTransition(TcpChannel::TCPCH_IDLE,
                                        TcpChannel::TCPCH_RESOLVING));
    CHECK(TcpChannel::IsLegalTransition(TcpChannel::TCPCH_RESOLVING,
                                        TcpChannel::TCPCH_PINNING));
    CHECK(TcpChannel::IsLegalTransition(TcpChannel::TCPCH_PINNING,
                                        TcpChannel::TCPCH_CONNECTING));
    CHECK(TcpChannel::IsLegalTransition(TcpChannel::TCPCH_CONNECTING,
                                        TcpChannel::TCPCH_TLS_HANDSHAKE));
    CHECK(TcpChannel::IsLegalTransition(TcpChannel::TCPCH_TLS_HANDSHAKE,
                                        TcpChannel::TCPCH_READY));
    // 明文路径：CONNECTING 直接进 READY，跳过 TLS_HANDSHAKE
    CHECK(TcpChannel::IsLegalTransition(TcpChannel::TCPCH_CONNECTING,
                                        TcpChannel::TCPCH_READY));
}

static void test_pinning_cannot_be_skipped()
{
    // 这是整个状态机最重要的一条约束：绑网卡必须在 connect 之前完整走完。
    // macOS 的 IP_BOUND_IF / Linux 的 SO_BINDTODEVICE / Windows 的
    // IP_UNICAST_IF 在 connect 之后设置都不生效。
    CHECK(!TcpChannel::IsLegalTransition(TcpChannel::TCPCH_RESOLVING,
                                         TcpChannel::TCPCH_CONNECTING));
    CHECK(!TcpChannel::IsLegalTransition(TcpChannel::TCPCH_IDLE,
                                         TcpChannel::TCPCH_CONNECTING));
    CHECK(!TcpChannel::IsLegalTransition(TcpChannel::TCPCH_IDLE,
                                         TcpChannel::TCPCH_PINNING));
    // 也不允许绕过 DNS 直接开绑
    CHECK(!TcpChannel::IsLegalTransition(TcpChannel::TCPCH_IDLE,
                                         TcpChannel::TCPCH_READY));
}

static void test_no_backwards_transitions()
{
    CHECK(!TcpChannel::IsLegalTransition(TcpChannel::TCPCH_READY,
                                         TcpChannel::TCPCH_CONNECTING));
    CHECK(!TcpChannel::IsLegalTransition(TcpChannel::TCPCH_CONNECTING,
                                         TcpChannel::TCPCH_PINNING));
    CHECK(!TcpChannel::IsLegalTransition(TcpChannel::TCPCH_TLS_HANDSHAKE,
                                         TcpChannel::TCPCH_CONNECTING));
    CHECK(!TcpChannel::IsLegalTransition(TcpChannel::TCPCH_READY,
                                         TcpChannel::TCPCH_READY));
}

static void test_closing_transitions()
{
    const TcpChannel::State kLive[] = {
        TcpChannel::TCPCH_IDLE,          TcpChannel::TCPCH_RESOLVING,
        TcpChannel::TCPCH_PINNING,       TcpChannel::TCPCH_CONNECTING,
        TcpChannel::TCPCH_TLS_HANDSHAKE, TcpChannel::TCPCH_READY,
    };
    // 任何活动状态都能被打断进入 CLOSING（超时 / 对端断开 / 主动 Close）
    for (size_t i = 0; i < sizeof(kLive) / sizeof(kLive[0]); i++)
    {
        CHECK(TcpChannel::IsLegalTransition(kLive[i], TcpChannel::TCPCH_CLOSING));
    }
    // CLOSING 只能走到 CLOSED；CLOSED 是终态
    CHECK(TcpChannel::IsLegalTransition(TcpChannel::TCPCH_CLOSING,
                                        TcpChannel::TCPCH_CLOSED));
    CHECK(!TcpChannel::IsLegalTransition(TcpChannel::TCPCH_CLOSING,
                                         TcpChannel::TCPCH_CLOSING));
    CHECK(!TcpChannel::IsLegalTransition(TcpChannel::TCPCH_CLOSED,
                                         TcpChannel::TCPCH_CLOSING));
    CHECK(!TcpChannel::IsLegalTransition(TcpChannel::TCPCH_CLOSED,
                                         TcpChannel::TCPCH_READY));
    CHECK(!TcpChannel::IsLegalTransition(TcpChannel::TCPCH_CLOSING,
                                         TcpChannel::TCPCH_READY));
    // 没有 CLOSING 直接不进 CLOSED 的捷径
    CHECK(!TcpChannel::IsLegalTransition(TcpChannel::TCPCH_READY,
                                         TcpChannel::TCPCH_CLOSED));
}

///////////////////////////////////////////////////////////////////////////////
// IP 字面量解析
///////////////////////////////////////////////////////////////////////////////

static void test_parse_ipv4_literal()
{
    BCSockAddrS sa;
    memset(&sa, 0, sizeof(sa));

    CHECK(TcpChannel::ParseIpLiteral("93.184.216.34", 443, sa));
    CHECK(sa.type.sa.sa_family == AF_INET);
    CHECK(ntohs(sa.type.sin.sin_port) == 443);
    CHECK(ntohl(sa.type.sin.sin_addr.s_addr) == 0x5DB8D822u);
    CHECK(sa.length == sizeof(sa.type.sin));
}

static void test_parse_ipv6_literal()
{
    BCSockAddrS sa;
    memset(&sa, 0, sizeof(sa));

    CHECK(TcpChannel::ParseIpLiteral("2606:2800:220:1:248:1893:25c8:1946",
                                     8443, sa));
    CHECK(sa.type.sa.sa_family == AF_INET6);
    CHECK(ntohs(sa.type.sin6.sin6_port) == 8443);
    CHECK(sa.type.sin6.sin6_addr.s6_addr[0] == 0x26);
    CHECK(sa.type.sin6.sin6_addr.s6_addr[1] == 0x06);

    // 压缩写法同样要认
    memset(&sa, 0, sizeof(sa));
    CHECK(TcpChannel::ParseIpLiteral("::1", 80, sa));
    CHECK(sa.type.sa.sa_family == AF_INET6);
    CHECK(sa.type.sin6.sin6_addr.s6_addr[15] == 1);
}

static void test_parse_rejects_non_literals()
{
    BCSockAddrS sa;

    // 域名不是字面量 —— resolvedIp 填域名必须被拒，否则会拿域名去当 IP 用
    CHECK(!TcpChannel::ParseIpLiteral("example.com", 443, sa));
    CHECK(!TcpChannel::ParseIpLiteral("", 443, sa));
    CHECK(!TcpChannel::ParseIpLiteral("1.2.3", 443, sa));
    CHECK(!TcpChannel::ParseIpLiteral("1.2.3.4.5", 443, sa));
    CHECK(!TcpChannel::ParseIpLiteral("256.1.1.1", 443, sa));
    CHECK(!TcpChannel::ParseIpLiteral("1.2.3.4:443", 443, sa));
    // 带端口/方括号的 authority 形式也不是字面量，调用方要自己拆好再传
    CHECK(!TcpChannel::ParseIpLiteral("[::1]", 443, sa));
    CHECK(!TcpChannel::ParseIpLiteral("gg::1", 443, sa));
}

///////////////////////////////////////////////////////////////////////////////
// Open() 的同步校验
//
// 下面这些用例全部在 BCEventQueue::Create 之前就返回，所以不需要 Runtime。
///////////////////////////////////////////////////////////////////////////////

class NullHandler : public ITcpChannelHandler
{
public:
    int ready = 0, data = 0, closed = 0;

    void OnChannelReady() override { ready++; }
    void OnChannelData(const void*, size_t) override { data++; }
    void OnChannelClosed(BCRESULT, const std::string&) override { closed++; }
};

static TcpChannelConfig MakeConfig()
{
    TcpChannelConfig cfg;
    cfg.host             = "example.com";
    cfg.port             = 443;
    cfg.tls              = true;
    cfg.connectTimeoutMs = 5000;
    return cfg;
}

static void test_open_rejects_bad_arguments()
{
    NullHandler h;

    {
        TcpChannel ch;
        CHECK(ch.Open(MakeConfig(), NULL) == BC_R_INVALIDARG);
    }
    {
        TcpChannel ch;
        TcpChannelConfig cfg = MakeConfig();
        cfg.host.clear();
        CHECK(ch.Open(cfg, &h) == BC_R_INVALIDARG);
    }
    {
        TcpChannel ch;
        TcpChannelConfig cfg = MakeConfig();
        cfg.port = 0;
        CHECK(ch.Open(cfg, &h) == BC_R_INVALIDARG);
    }
    // 参数被拒时不能有任何回调 —— Open 返回错误即代表"什么都没发生"
    CHECK(h.ready == 0 && h.data == 0 && h.closed == 0);
}

static void test_open_rejects_non_literal_resolved_ip()
{
    NullHandler h;
    TcpChannel  ch;
    TcpChannelConfig cfg = MakeConfig();

    cfg.policy     = TT_VPN_POLICY_OS;      // 排除策略分支的干扰
    cfg.resolvedIp = "example.com";         // 不是 IP 字面量
    CHECK(ch.Open(cfg, &h) == BC_R_INVALIDARG);
    CHECK(h.closed == 0);
}

static void test_force_physical_fails_hard_without_interface()
{
    // 回归测试：本 TU 没有定义 TT_HAS_PATH_MONITOR，_PickPhysicalIfIndex()
    // 里那段自动探测被编译掉了，config.ifIndex 又是 0 —— 也就是"完全找不到
    // 物理网卡"。force-physical 此时必须以 BC_R_NO_PHYSICAL_INTERFACE 硬失败，
    // 绝不能因为 guard 关掉了就一路放行到 connect（Task 4 的那个 Critical）。
    NullHandler h;
    TcpChannel  ch;
    TcpChannelConfig cfg = MakeConfig();

    cfg.policy  = TT_VPN_POLICY_FORCE_PHYSICAL;
    cfg.ifIndex = 0;
    CHECK(ch.Open(cfg, &h) == BC_R_NO_PHYSICAL_INTERFACE);
    // 值 64 是跨语言契约（deps/env/src/BC/Config.h:388 / TTSignalConfig.swift /
    // src/js/index.js），钉住防止有人把它挪到 BC_R_NRESULTS + N 上去
    CHECK(BC_R_NO_PHYSICAL_INTERFACE == 64);
    CHECK(h.closed == 0);   // Open 返回错误码即已报告，不再重复回调
    // 同步失败走不到 _Fail()，现场描述只能靠 LastOpenError() 带出去 ——
    // 绑定层（Swift TTSignalError.errMessage / NAPI）唯一的信息来源。
    CHECK(!ch.LastOpenError().empty());
    CHECK(ch.LastOpenError().find("force-physical") != std::string::npos);
}

static void test_force_physical_passes_with_explicit_ifindex()
{
    // 调用方显式给了 ifIndex（Android 走 androidNetHandle 同理），
    // force-physical 的硬校验就该放行，往下走到 BCEventQueue::Create。
    // 本测试没初始化 Runtime，Create 会拿到 NULL 的 timer/task mgr 而返回
    // BC_R_INVALIDARG —— 关键是它**不是** BC_R_NO_PHYSICAL_INTERFACE。
    NullHandler h;
    TcpChannel  ch;
    TcpChannelConfig cfg = MakeConfig();

    cfg.policy  = TT_VPN_POLICY_FORCE_PHYSICAL;
    cfg.ifIndex = 7;
    BCRESULT r = ch.Open(cfg, &h);
    CHECK(r != BC_R_NO_PHYSICAL_INTERFACE);
    CHECK(r != BC_R_SUCCESS);   // 没有 Runtime，走不到真正建连
    CHECK(h.closed == 0);
}

static void test_prefer_physical_does_not_fail_without_interface()
{
    // prefer-physical 找不到物理网卡时只打 WARN 回落系统路由，不硬失败。
    NullHandler h;
    TcpChannel  ch;
    TcpChannelConfig cfg = MakeConfig();

    cfg.policy  = TT_VPN_POLICY_PREFER_PHYSICAL;
    cfg.ifIndex = 0;
    CHECK(ch.Open(cfg, &h) != BC_R_NO_PHYSICAL_INTERFACE);
}

static void test_os_policy_never_checks_interface()
{
    NullHandler h;
    TcpChannel  ch;
    TcpChannelConfig cfg = MakeConfig();

    cfg.policy  = TT_VPN_POLICY_OS;
    cfg.ifIndex = 0;
    CHECK(ch.Open(cfg, &h) != BC_R_NO_PHYSICAL_INTERFACE);
}

static void test_initial_state_and_diagnostics()
{
    TcpChannel ch;

    CHECK(ch.GetState() == TcpChannel::TCPCH_IDLE);
    CHECK(ch.BoundIfIndex() == 0);
    CHECK(ch.PeerIp().empty());
    CHECK(ch.PinMethod().empty());
    // 没 Open 过就 Send / Close 不能崩，也不能假装成功
    CHECK(ch.Send(BufferPtr()) == BC_R_INVALIDARG);
    CHECK(ch.Send(BufferPtr(new BCBuffer)) == BC_R_NOTCONNECTED);
    ch.Close();
    CHECK(ch.GetState() == TcpChannel::TCPCH_IDLE);
}

///////////////////////////////////////////////////////////////////////////////
// main
///////////////////////////////////////////////////////////////////////////////

int main()
{
    test_happy_path_transitions();
    test_pinning_cannot_be_skipped();
    test_no_backwards_transitions();
    test_closing_transitions();

    test_parse_ipv4_literal();
    test_parse_ipv6_literal();
    test_parse_rejects_non_literals();

    test_open_rejects_bad_arguments();
    test_open_rejects_non_literal_resolved_ip();
    test_force_physical_fails_hard_without_interface();
    test_force_physical_passes_with_explicit_ifindex();
    test_prefer_physical_does_not_fail_without_interface();
    test_os_policy_never_checks_interface();
    test_initial_state_and_diagnostics();

    if (g_failures == 0)
    {
        printf("TcpChannel_test: ALL PASSED\n");
        return 0;
    }
    printf("TcpChannel_test: %d FAILURE(S)\n", g_failures);
    return 1;
}

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
