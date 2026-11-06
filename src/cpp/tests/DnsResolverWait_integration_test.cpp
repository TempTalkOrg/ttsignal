


#include "DnsResolver.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)






enum FakeReply
{
    kReplyAddr,    
    kReplyEmpty,   
    kReplySilent,  
};

class FakeDnsServer
{
public:
    FakeDnsServer(FakeReply onA, FakeReply onAAAA)
        : on_a_(onA), on_aaaa_(onAAAA), fd_(-1), port_(0), stop_(false)
    {
        fd_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd_ < 0) return;

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port        = 0;   
        if (bind(fd_, (struct sockaddr*)&addr, sizeof(addr)) != 0) return;

        socklen_t len = sizeof(addr);
        if (getsockname(fd_, (struct sockaddr*)&addr, &len) != 0) return;
        port_ = ntohs(addr.sin_port);

        
        
        
        struct timeval tv;
        tv.tv_sec  = 0;
        tv.tv_usec = 100 * 1000;
        setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        thread_ = std::thread([this] { this->_Loop(); });
    }

    ~FakeDnsServer()
    {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
        if (fd_ >= 0) close(fd_);
    }

    uint16_t Port() const { return port_; }
    int      QueriesSeen() const { return queries_seen_.load(); }

private:
    void _Loop()
    {
        uint8_t buf[512];
        while (!stop_)
        {
            struct sockaddr_in from;
            socklen_t          fromLen = sizeof(from);
            ssize_t n = recvfrom(fd_, buf, sizeof(buf), 0,
                                 (struct sockaddr*)&from, &fromLen);
            if (n < 12) continue;   
            queries_seen_++;

            
            uint16_t qtype = (uint16_t)((buf[n - 4] << 8) | buf[n - 3]);
            FakeReply how = (qtype == 28) ? on_aaaa_ : on_a_;
            if (how == kReplySilent) continue;

            
            std::vector<uint8_t> resp(buf, buf + n);
            resp[2] = 0x81;   
            resp[3] = 0x80;   
            if (how == kReplyAddr)
            {
                resp[6] = 0x00; resp[7] = 0x01;   

                resp.push_back(0xC0); resp.push_back(0x0C);   
                if (qtype == 28)
                {
                    resp.push_back(0x00); resp.push_back(0x1C);   
                }
                else
                {
                    resp.push_back(0x00); resp.push_back(0x01);   
                }
                resp.push_back(0x00); resp.push_back(0x01);       
                resp.push_back(0x00); resp.push_back(0x00);
                resp.push_back(0x01); resp.push_back(0x2C);       

                if (qtype == 28)
                {
                    resp.push_back(0x00); resp.push_back(0x10);   
                    
                    const uint8_t v6[16] = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0,
                                            0, 0, 0, 0, 0, 0, 0, 0x01};
                    resp.insert(resp.end(), v6, v6 + 16);
                }
                else
                {
                    resp.push_back(0x00); resp.push_back(0x04);   
                    resp.push_back(203); resp.push_back(0);
                    resp.push_back(113); resp.push_back(7);       
                }
            }
            sendto(fd_, resp.data(), resp.size(), 0,
                   (struct sockaddr*)&from, fromLen);
        }
    }

    FakeReply        on_a_;
    FakeReply        on_aaaa_;
    int              fd_;
    uint16_t         port_;
    std::atomic<bool> stop_;
    std::atomic<int>  queries_seen_{0};
    std::thread      thread_;
};








static DnsConfig MakeCfg(const FakeDnsServer& server)
{
    DnsConfig cfg;
    cfg.policy     = TT_VPN_POLICY_PREFER_PHYSICAL;
    cfg.servers    = {"127.0.0.1"};
    cfg.serverPort = server.Port();
    cfg.timeoutMs  = 1000;
    return cfg;
}

static bool HasV4(const std::vector<BCSockAddrS>& addrs, const char* want)
{
    for (size_t i = 0; i < addrs.size(); i++)
    {
        if (addrs[i].type.sa.sa_family != AF_INET) continue;
        char buf[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &addrs[i].type.sin.sin_addr, buf, sizeof(buf));
        if (strcmp(buf, want) == 0) return true;
    }
    return false;
}

static bool HasV6(const std::vector<BCSockAddrS>& addrs, const char* want)
{
    for (size_t i = 0; i < addrs.size(); i++)
    {
        if (addrs[i].type.sa.sa_family != AF_INET6) continue;
        char buf[INET6_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET6, &addrs[i].type.sin6.sin6_addr, buf, sizeof(buf));
        if (strcmp(buf, want) == 0) return true;
    }
    return false;
}

static long ElapsedMs(std::chrono::steady_clock::time_point t0)
{
    return (long)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
}







static void test_both_answers_are_collected()
{
    FakeDnsServer server(kReplyAddr, kReplyAddr);
    CHECK(server.Port() != 0);

    DnsResolver::ClearCache();
    DnsResult r = DnsResolver::Resolve("both.test.invalid", MakeCfg(server));

    CHECK(r.result == BC_R_SUCCESS);
    CHECK(HasV4(r.addrs, "203.0.113.7"));
    CHECK(HasV6(r.addrs, "2001:db8::1"));
}



static void test_a_answer_does_not_wait_for_missing_aaaa()
{
    FakeDnsServer server(kReplyAddr, kReplySilent);
    CHECK(server.Port() != 0);

    DnsConfig cfg = MakeCfg(server);   

    DnsResolver::ClearCache();
    auto      t0 = std::chrono::steady_clock::now();
    DnsResult r  = DnsResolver::Resolve("aonly.test.invalid", cfg);
    long      ms = ElapsedMs(t0);

    CHECK(r.result == BC_R_SUCCESS);
    CHECK(HasV4(r.addrs, "203.0.113.7"));
    
    
    if (ms >= 300) printf("      实测耗时 %ld ms\n", ms);
    CHECK(ms < 300);
}


static void test_aaaa_answer_does_not_wait_for_missing_a()
{
    FakeDnsServer server(kReplySilent, kReplyAddr);
    CHECK(server.Port() != 0);

    DnsResolver::ClearCache();
    auto      t0 = std::chrono::steady_clock::now();
    DnsResult r  = DnsResolver::Resolve("v6only.test.invalid", MakeCfg(server));
    long      ms = ElapsedMs(t0);

    CHECK(r.result == BC_R_SUCCESS);
    CHECK(HasV6(r.addrs, "2001:db8::1"));
    if (ms >= 300) printf("      实测耗时 %ld ms\n", ms);
    CHECK(ms < 300);
}




static void test_empty_answers_still_wait_full_timeout()
{
    FakeDnsServer server(kReplyEmpty, kReplySilent);
    CHECK(server.Port() != 0);

    DnsConfig cfg = MakeCfg(server);   

    DnsResolver::ClearCache();
    auto      t0 = std::chrono::steady_clock::now();
    DnsResult r  = DnsResolver::Resolve("empty.test.invalid", cfg);
    long      ms = ElapsedMs(t0);

    
    
    CHECK(r.result != BC_R_SUCCESS);
    if (ms < 900) printf("      实测耗时 %ld ms\n", ms);
    CHECK(ms >= 900);
}








static void test_config_defaults()
{
    DnsConfig cfg;

    CHECK(cfg.timeoutMs == 1000);
    CHECK(cfg.secondAnswerGraceMs == 50);
    CHECK(cfg.serverPort == 53);
}



int main()
{
    test_config_defaults();
    test_both_answers_are_collected();
    test_a_answer_does_not_wait_for_missing_aaaa();
    test_aaaa_answer_does_not_wait_for_missing_a();
    test_empty_answers_still_wait_full_timeout();

    if (g_failures == 0)
    {
        printf("DnsResolverWait_integration_test: all passed\n");
        return 0;
    }
    printf("DnsResolverWait_integration_test: %d failure(s)\n", g_failures);
    return 1;
}




