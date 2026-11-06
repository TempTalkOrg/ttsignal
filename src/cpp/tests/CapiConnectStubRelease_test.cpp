



#include "ios_bridge.h"
#include "ios_bridge_internal.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

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

struct State {
    std::mutex mu;
    std::condition_variable cv;
    std::vector<int32_t> results;
    bool closed = false;

    bool wait(int ms, size_t nresults) {
        std::unique_lock<std::mutex> lk(mu);
        return cv.wait_for(lk, std::chrono::milliseconds(ms), [&] { return results.size() >= nresults; });
    }
    bool wait_closed(int ms) {
        std::unique_lock<std::mutex> lk(mu);
        return cv.wait_for(lk, std::chrono::milliseconds(ms), [&] { return closed; });
    }
};

static void on_connect_result(void* ud, int32_t err, const char*) {
    auto* st = static_cast<State*>(ud);
    std::lock_guard<std::mutex> lk(st->mu);
    st->results.push_back(err);
    st->cv.notify_all();
}

static void on_closed(void* ud, const char*) {
    auto* st = static_cast<State*>(ud);
    std::lock_guard<std::mutex> lk(st->mu);
    st->closed = true;
    st->cv.notify_all();
}


static int live_after_settle() {
    for (int i = 0; i < 50 && ttsignal_capi_internal::ConnectStubLiveCount() != 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return ttsignal_capi_internal::ConnectStubLiveCount();
}

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);
    TTConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.alpn = "ttsignal";
    cfg.serverHost = "localhost";
    cfg.idleTimeOut = 20000;
    cfg.maxConnections = 1000;
    cfg.congestCtrl = 'B';
    cfg.pingInterval = 10000;
    cfg.activeConnectionIdLimit = 1000;
    cfg.logLevel = 4;
    cfg.numOfSenders = 1;
    cfg.disableAutoRestart = 1;
    cfg.bypassVpn = -1;
    cfg.vpnPolicy = -1;

    TTHandlerVTable vt;
    memset(&vt, 0, sizeof(vt));
    vt.on_connect_result = on_connect_result;
    vt.on_closed = on_closed;

    TTConnectorRef connector = tt_connector_create(&cfg);
    if (!connector) {
        printf("tt_connector_create 失败：%s\n", tt_last_sync_error());
        return 1;
    }

    for (int round = 0; round < 3; ++round) {
        printf("[round %d] connect 超时 + 重复 connect\n", round);
        State st;
        TTConnectionRef conn = tt_connector_create_connection(connector, &cfg, &vt, &st);
        EXPECT(conn != NULL, "create_connection 失败");
        if (!conn) break;
        int32_t r = tt_connection_connect(conn, "https://127.0.0.1:9/proxy", NULL, 300);
        EXPECT(r == 0, "第一次 connect = %d", r);
        r = tt_connection_connect(conn, "https://127.0.0.1:9/proxy", NULL, 300);  
        EXPECT(r == 0, "第二次 connect = %d", r);
        EXPECT(st.wait(15000, 2), "15 s 内未收齐两次 on_connect_result（收到 %zu 次）", st.results.size());
        for (int32_t e : st.results) printf("  on_connect_result: %d (%s)\n", e, tt_result_name(e));
        for (int32_t e : st.results) EXPECT(e != 0, "connect 结果应为失败，得 %d", e);
        int live = live_after_settle();
        EXPECT(live == 0, "结果交付后 connect stub 存活数 %d，want 0（stub 泄漏）", live);
        tt_connection_close(conn);
        EXPECT(st.wait_closed(5000), "5 s 内未收到 on_closed");
        tt_connection_destroy(conn);
    }

    int32_t r = tt_connector_close_sync(connector, 5000);
    EXPECT(r == 0, "tt_connector_close_sync = %d", r);
    tt_connector_destroy(connector);

    
    for (int i = 0; i < 5; ++i) {
        TTConnectorRef c = tt_connector_create(&cfg);
        EXPECT(c != NULL, "第 %d 次 tt_connector_create 失败", i);
        if (!c) break;
        EXPECT(tt_connector_close_sync(c, 5000) == 0, "第 %d 次 close_sync 失败", i);
        tt_connector_destroy(c);
    }
    printf(g_failures ? "FAILED（%d）\n" : "PASS\n", g_failures);
    return g_failures ? 1 : 0;
}
