


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





static void test_happy_path_transitions()
{
    
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
    
    CHECK(TcpChannel::IsLegalTransition(TcpChannel::TCPCH_CONNECTING,
                                        TcpChannel::TCPCH_READY));
}

static void test_pinning_cannot_be_skipped()
{
    
    
    
    CHECK(!TcpChannel::IsLegalTransition(TcpChannel::TCPCH_RESOLVING,
                                         TcpChannel::TCPCH_CONNECTING));
    CHECK(!TcpChannel::IsLegalTransition(TcpChannel::TCPCH_IDLE,
                                         TcpChannel::TCPCH_CONNECTING));
    CHECK(!TcpChannel::IsLegalTransition(TcpChannel::TCPCH_IDLE,
                                         TcpChannel::TCPCH_PINNING));
    
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
    
    for (size_t i = 0; i < sizeof(kLive) / sizeof(kLive[0]); i++)
    {
        CHECK(TcpChannel::IsLegalTransition(kLive[i], TcpChannel::TCPCH_CLOSING));
    }
    
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
    
    CHECK(!TcpChannel::IsLegalTransition(TcpChannel::TCPCH_READY,
                                         TcpChannel::TCPCH_CLOSED));
}





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

    
    memset(&sa, 0, sizeof(sa));
    CHECK(TcpChannel::ParseIpLiteral("::1", 80, sa));
    CHECK(sa.type.sa.sa_family == AF_INET6);
    CHECK(sa.type.sin6.sin6_addr.s6_addr[15] == 1);
}

static void test_parse_rejects_non_literals()
{
    BCSockAddrS sa;

    
    CHECK(!TcpChannel::ParseIpLiteral("example.com", 443, sa));
    CHECK(!TcpChannel::ParseIpLiteral("", 443, sa));
    CHECK(!TcpChannel::ParseIpLiteral("1.2.3", 443, sa));
    CHECK(!TcpChannel::ParseIpLiteral("1.2.3.4.5", 443, sa));
    CHECK(!TcpChannel::ParseIpLiteral("256.1.1.1", 443, sa));
    CHECK(!TcpChannel::ParseIpLiteral("1.2.3.4:443", 443, sa));
    
    CHECK(!TcpChannel::ParseIpLiteral("[::1]", 443, sa));
    CHECK(!TcpChannel::ParseIpLiteral("gg::1", 443, sa));
}







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
    
    CHECK(h.ready == 0 && h.data == 0 && h.closed == 0);
}

static void test_open_rejects_non_literal_resolved_ip()
{
    NullHandler h;
    TcpChannel  ch;
    TcpChannelConfig cfg = MakeConfig();

    cfg.policy     = TT_VPN_POLICY_OS;      
    cfg.resolvedIp = "example.com";         
    CHECK(ch.Open(cfg, &h) == BC_R_INVALIDARG);
    CHECK(h.closed == 0);
}

static void test_force_physical_fails_hard_without_interface()
{
    
    
    
    
    NullHandler h;
    TcpChannel  ch;
    TcpChannelConfig cfg = MakeConfig();

    cfg.policy  = TT_VPN_POLICY_FORCE_PHYSICAL;
    cfg.ifIndex = 0;
    CHECK(ch.Open(cfg, &h) == BC_R_NO_PHYSICAL_INTERFACE);
    
    
    CHECK(BC_R_NO_PHYSICAL_INTERFACE == 64);
    CHECK(h.closed == 0);   
    
    
    CHECK(!ch.LastOpenError().empty());
    CHECK(ch.LastOpenError().find("force-physical") != std::string::npos);
}

static void test_force_physical_passes_with_explicit_ifindex()
{
    
    
    
    
    NullHandler h;
    TcpChannel  ch;
    TcpChannelConfig cfg = MakeConfig();

    cfg.policy  = TT_VPN_POLICY_FORCE_PHYSICAL;
    cfg.ifIndex = 7;
    BCRESULT r = ch.Open(cfg, &h);
    CHECK(r != BC_R_NO_PHYSICAL_INTERFACE);
    CHECK(r != BC_R_SUCCESS);   
    CHECK(h.closed == 0);
}

static void test_prefer_physical_does_not_fail_without_interface()
{
    
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
    
    CHECK(ch.Send(BufferPtr()) == BC_R_INVALIDARG);
    CHECK(ch.Send(BufferPtr(new BCBuffer)) == BC_R_NOTCONNECTED);
    ch.Close();
    CHECK(ch.GetState() == TcpChannel::TCPCH_IDLE);
}





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




