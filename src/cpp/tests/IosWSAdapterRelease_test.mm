///////////////////////////////////////////////////////////////////////////////
// file : IosWSAdapterRelease_test.mm
//
// ios_http_bridge.mm 里 TTWSConnAdapter 的销毁归属（「谁最后到谁销毁」）。
// 覆盖 ReleaseOnce（回调侧）与 DetachFromOwner（业务 destroy 侧）的四种交错，
// 重点是**并发**那一种 —— crash-2026.09.07.01 修完之后残留的 use-after-free：
//
//   回调线程 ReleaseOnce 放了锁、还没读完 vt/userdata，destroy 线程
//   DetachFromOwner 看到 released_ 已置位就 delete this，回调线程于是在已释放
//   的 adapter 上继续跑。
//
// 用 on_release 里回读 adapter 把那个窗口坐实：测试把 userdata 设成 adapter
// 自身，回调阻塞在里面时让另一条线程走 DetachFromOwner，然后回调再摸一下
// adapter。ASan 会在"另一条线程已经 delete"时报 heap-use-after-free。
// 修好之后 DetachFromOwner 必须让路，由 ReleaseOnce 收尾 —— 这个读就一定合法。
//
// TTWSConnAdapter 是 .mm 内部类型（没有头文件导出），所以这里 unity-include
// 整个 .mm。链接需要的一大串源文件同 WSConnector_test.cpp。
//
// ---------------------------------------------------------------------------
// 怎么跑（macOS arm64）
// ---------------------------------------------------------------------------
//
//   cd /Users/antonio/chative/ttsignal
//
//   for f in api http llhttp; do \
//       cc -c -g -fsanitize=address -I deps/llhttp/include \
//          deps/llhttp/src/$f.c -o /tmp/llhttp_$f.o || break; done
//
//   c++ -std=c++17 -g -Wall -fsanitize=address -fno-omit-frame-pointer \
//       -I src/cpp -I src/cpp/apple -I deps/env/src -I deps/jquic/include \
//       -I deps/boringssl/src/include -I deps/llhttp/include \
//       src/cpp/VpnPolicy.cpp src/cpp/SocketPinner.cpp src/cpp/DnsResolver.cpp \
//       src/cpp/TlsContext.cpp src/cpp/SSLayer.cpp src/cpp/Runtime.cpp \
//       src/cpp/NetworkRouteLookup.cpp src/cpp/apple/AppleRouteLookup.cpp \
//       src/cpp/TcpChannel.cpp src/cpp/Utils.cpp \
//       src/cpp/SMPacket.cpp src/cpp/SMPParser.cpp \
//       src/cpp/LLHTTPParser.cpp src/cpp/HttpConnector.cpp \
//       src/cpp/WSParser.cpp src/cpp/WSConnector.cpp \
//       /tmp/llhttp_api.o /tmp/llhttp_http.o /tmp/llhttp_llhttp.o \
//       src/cpp/tests/IosWSAdapterRelease_test.mm \
//       deps/env/lib/Darwin/arm64/Debug/libenv.a \
//       deps/boringssl/lib/Darwin/arm64/Debug/libssl.a \
//       deps/boringssl/lib/Darwin/arm64/Debug/libcrypto.a \
//       -framework Foundation -framework Network -framework SystemConfiguration \
//       -o /tmp/ioswsadapter_ut
//   /tmp/ioswsadapter_ut
//
// 本文件一行网络代码都没有，上面那一大串源文件只是为了满足链接器。
///////////////////////////////////////////////////////////////////////////////

#include "apple/ios_http_bridge.mm"

#include <condition_variable>
#include <thread>
#include <cstdio>

using ios_http_bridge::TTWSConnAdapter;

static int g_failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

namespace {

int g_release_calls = 0;

void CountingRelease(void*)
{
    ++g_release_calls;
}

TTWSConnAdapter* MakeAdapter(void (*onRelease)(void*), void* ud)
{
    TTWSConnAdapter* ad = new TTWSConnAdapter();
    memset(&ad->vt, 0, sizeof(ad->vt));
    ad->vt.on_release = onRelease;
    ad->userdata      = ud;
    // conn 留空：本文件只测销毁归属，不建连接。DetachFromOwner / ReleaseOnce
    // 都不碰 conn。
    return ad;
}

///////////////////////////////////////////////////////////////////////////////
// 1) 从没受理过 Connect：契约 1 说不会有任何回调，destroy 就地收尾。
///////////////////////////////////////////////////////////////////////////////
void TestDetachNeverConnected()
{
    printf("TestDetachNeverConnected\n");
    g_release_calls = 0;
    TTWSConnAdapter* ad = MakeAdapter(CountingRelease, nullptr);
    ad->DetachFromOwner(/*connectIssued=*/false);   // 内部 delete
    CHECK(g_release_calls == 1);
}

///////////////////////////////////////////////////////////////////////////////
// 2) 回调先到、业务后放手：ReleaseOnce 不删，DetachFromOwner 删。
///////////////////////////////////////////////////////////////////////////////
void TestReleaseThenDetach()
{
    printf("TestReleaseThenDetach\n");
    g_release_calls = 0;
    TTWSConnAdapter* ad = MakeAdapter(CountingRelease, nullptr);
    ad->ReleaseOnce();
    CHECK(g_release_calls == 1);
    ad->ReleaseOnce();                              // 幂等
    CHECK(g_release_calls == 1);
    ad->DetachFromOwner(/*connectIssued=*/true);    // 内部 delete
    CHECK(g_release_calls == 1);
}

///////////////////////////////////////////////////////////////////////////////
// 3) 业务先放手、回调后到：DetachFromOwner 不删，ReleaseOnce 删。
///////////////////////////////////////////////////////////////////////////////
void TestDetachThenRelease()
{
    printf("TestDetachThenRelease\n");
    g_release_calls = 0;
    TTWSConnAdapter* ad = MakeAdapter(CountingRelease, nullptr);
    ad->DetachFromOwner(/*connectIssued=*/true);    // 回调在途，不能删
    CHECK(g_release_calls == 0);
    ad->ReleaseOnce();                              // 内部 delete
    CHECK(g_release_calls == 1);
}

///////////////////////////////////////////////////////////////////////////////
// 4) 并发：ReleaseOnce 正在跑 on_release，业务同时 destroy。
//
//    这就是残留的 UAF。on_release 阻塞在里面时，另一条线程走
//    DetachFromOwner —— 它若在此刻 delete，回调返回前对 adapter 的任何访问
//    都踩在已释放内存上。
///////////////////////////////////////////////////////////////////////////////

std::mutex              g_mu;
std::condition_variable g_cv;
bool                    g_in_release = false;   // on_release 已进入
bool                    g_may_finish = false;   // 允许 on_release 返回

void BlockingRelease(void* ud)
{
    {
        std::unique_lock<std::mutex> lk(g_mu);
        ++g_release_calls;
        g_in_release = true;
        g_cv.notify_all();
        g_cv.wait(lk, [] { return g_may_finish; });
    }
    // ⚠️ 关键断言点：ReleaseOnce 还没返回，adapter 必须还活着。
    // 老代码里 DetachFromOwner 已经 delete 了它，ASan 在这里报
    // heap-use-after-free。
    TTWSConnAdapter* ad = static_cast<TTWSConnAdapter*>(ud);
    CHECK(ad->vt.on_release == &BlockingRelease);
}

void TestConcurrentReleaseAndDetach()
{
    printf("TestConcurrentReleaseAndDetach\n");
    g_release_calls = 0;
    g_in_release    = false;
    g_may_finish    = false;

    TTWSConnAdapter* ad = MakeAdapter(BlockingRelease, nullptr);
    ad->userdata        = ad;               // 让回调能回头摸 adapter

    std::thread cb([ad] { ad->ReleaseOnce(); });

    {   // 等回调真的进到 on_release 里
        std::unique_lock<std::mutex> lk(g_mu);
        g_cv.wait(lk, [] { return g_in_release; });
    }

    // 业务此刻放手。回调还在途中，这里绝不能 delete。
    ad->DetachFromOwner(/*connectIssued=*/true);

    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_may_finish = true;
    }
    g_cv.notify_all();
    cb.join();                              // ReleaseOnce 返回时才 delete
    CHECK(g_release_calls == 1);
}

///////////////////////////////////////////////////////////////////////////////
// 5) 重入：on_release 里业务顺链 destroy（同一条线程）。
//
//    Swift 侧真实存在这条路径 —— on_release 释放 box -> TTSignalWSConnection
//    deinit -> tt_ws_connection_destroy -> DetachFromOwner。此时 ReleaseOnce
//    还没返回，DetachFromOwner 同样不能删；而且 mu_ 是非递归锁，on_release
//    绝不能在锁内调，否则这里直接死锁。
///////////////////////////////////////////////////////////////////////////////

TTWSConnAdapter* g_reentrant_ad = nullptr;

void ReentrantRelease(void*)
{
    ++g_release_calls;
    g_reentrant_ad->DetachFromOwner(/*connectIssued=*/true);
}

void TestReleaseReentersDetach()
{
    printf("TestReleaseReentersDetach\n");
    g_release_calls = 0;
    TTWSConnAdapter* ad = MakeAdapter(ReentrantRelease, nullptr);
    g_reentrant_ad      = ad;
    ad->ReleaseOnce();              // 返回后 adapter 已销毁，且只销毁一次
    CHECK(g_release_calls == 1);
    g_reentrant_ad = nullptr;
}

} // namespace

int main()
{
    TestDetachNeverConnected();
    TestReleaseThenDetach();
    TestDetachThenRelease();
    TestConcurrentReleaseAndDetach();
    TestReleaseReentersDetach();

    if (g_failures == 0) {
        printf("\nALL PASS\n");
        return 0;
    }
    printf("\n%d FAILURE(S)\n", g_failures);
    return 1;
}
