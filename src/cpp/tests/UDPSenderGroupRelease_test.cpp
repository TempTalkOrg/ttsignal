



#include "UDPSenderGroup.h"
#include "UDPSender.h"
#include "Runtime.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>

static int g_failures = 0;

#define EXPECT(cond, ...)                                  \
    do {                                                   \
        if (!(cond)) {                                     \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);  \
            printf(__VA_ARGS__);                           \
            printf("\n");                                  \
            g_failures++;                                  \
        }                                                  \
    } while (0)


class ClosedHandler : public IUDPSenderHandler
{
public:
    void OnSendData(uint32_t, UDPSender*) override {}
    void OnRecvData(BCBuffer*, BCSockAddrS&) override {}
    void OnCheckAvailable() override {}
    void OnRestart(BCRESULT) override {}
    void OnUdpClosed() override
    {
        std::lock_guard<std::mutex> lk(mu_);
        closed_ = true;
        cv_.notify_all();
    }
    bool WaitClosed(int ms)
    {
        std::unique_lock<std::mutex> lk(mu_);
        return cv_.wait_for(lk, std::chrono::milliseconds(ms), [&] { return closed_; });
    }

private:
    std::mutex mu_;
    std::condition_variable cv_;
    bool closed_ = false;
};

static UDPSenderGroup* NewGroup(IUDPSenderHandler* handler, int senders)
{
    BCFObject cfg;
    cfg.PutInt("num_of_senders", senders);
    UDPSenderGroup* g = new UDPSenderGroup();
    BCRESULT r = g->Create(NULL, Runtime::TaskMgr(), Runtime::TimerMgr(), Runtime::SocketMgr(), &cfg, handler,
                           false, false, 0);
    EXPECT(r == BC_R_SUCCESS, "UDPSenderGroup::Create = %d", r);
    return g;
}

static void case_destroy_releases_senders()
{
    printf("[destroy_releases_senders]\n");
    const int base = UDPSender::LiveCount();
    ClosedHandler h;
    UDPSenderGroup* g = NewGroup(&h, 3);
    EXPECT(UDPSender::LiveCount() == base + 3, "建组后实例数 %d，want %d", UDPSender::LiveCount(), base + 3);
    g->Destroy(&g);
    EXPECT(g == NULL, "Destroy 后指针应置空");
    EXPECT(UDPSender::LiveCount() == base, "Destroy 后实例数 %d，want %d", UDPSender::LiveCount(), base);
}

static void case_close_then_delete_releases_senders()
{
    printf("[close_then_delete_releases_senders]\n");
    const int base = UDPSender::LiveCount();
    ClosedHandler h;
    UDPSenderGroup* g = NewGroup(&h, 3);
    g->Close();
    EXPECT(h.WaitClosed(5000), "5 s 内未收到 OnUdpClosed");
    
    std::mutex mu;
    std::condition_variable cv;
    bool done = false;
    Runtime::PostTask([&] {
        BC_SAFE_DELETE_PTR(g);
        std::lock_guard<std::mutex> lk(mu);
        done = true;
        cv.notify_all();
    });
    {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait_for(lk, std::chrono::seconds(5), [&] { return done; });
    }
    EXPECT(done, "delete 任务 5 s 内未执行");
    EXPECT(UDPSender::LiveCount() == base, "关闭后 delete 组，实例数 %d，want %d（sender 泄漏）", UDPSender::LiveCount(), base);
}

int main()
{
    setvbuf(stdout, NULL, _IONBF, 0);
    BCFObject runtimeCfg;
    runtimeCfg.PutInt("workerThreads", 1);
    runtimeCfg.PutInt("taskThreads", 2);
    runtimeCfg.PutInt("timerThreads", 1);
    if (Runtime::Initialize(&runtimeCfg) != BC_R_SUCCESS)
    {
        printf("Runtime::Initialize 失败\n");
        return 1;
    }
    case_destroy_releases_senders();
    case_close_then_delete_releases_senders();
    Runtime::Destroy();
    printf(g_failures ? "FAILED（%d）\n" : "PASS\n", g_failures);
    return g_failures ? 1 : 0;
}
