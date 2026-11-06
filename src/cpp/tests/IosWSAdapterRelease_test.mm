



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
    
    
    return ad;
}




void TestDetachNeverConnected()
{
    printf("TestDetachNeverConnected\n");
    g_release_calls = 0;
    TTWSConnAdapter* ad = MakeAdapter(CountingRelease, nullptr);
    ad->DetachFromOwner(false);   
    CHECK(g_release_calls == 1);
}




void TestReleaseThenDetach()
{
    printf("TestReleaseThenDetach\n");
    g_release_calls = 0;
    TTWSConnAdapter* ad = MakeAdapter(CountingRelease, nullptr);
    ad->ReleaseOnce();
    CHECK(g_release_calls == 1);
    ad->ReleaseOnce();                              
    CHECK(g_release_calls == 1);
    ad->DetachFromOwner(true);    
    CHECK(g_release_calls == 1);
}




void TestDetachThenRelease()
{
    printf("TestDetachThenRelease\n");
    g_release_calls = 0;
    TTWSConnAdapter* ad = MakeAdapter(CountingRelease, nullptr);
    ad->DetachFromOwner(true);    
    CHECK(g_release_calls == 0);
    ad->ReleaseOnce();                              
    CHECK(g_release_calls == 1);
}









std::mutex              g_mu;
std::condition_variable g_cv;
bool                    g_in_release = false;   
bool                    g_may_finish = false;   

void BlockingRelease(void* ud)
{
    {
        std::unique_lock<std::mutex> lk(g_mu);
        ++g_release_calls;
        g_in_release = true;
        g_cv.notify_all();
        g_cv.wait(lk, [] { return g_may_finish; });
    }
    
    
    
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
    ad->userdata        = ad;               

    std::thread cb([ad] { ad->ReleaseOnce(); });

    {   
        std::unique_lock<std::mutex> lk(g_mu);
        g_cv.wait(lk, [] { return g_in_release; });
    }

    
    ad->DetachFromOwner(true);

    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_may_finish = true;
    }
    g_cv.notify_all();
    cb.join();                              
    CHECK(g_release_calls == 1);
}










TTWSConnAdapter* g_reentrant_ad = nullptr;

void ReentrantRelease(void*)
{
    ++g_release_calls;
    g_reentrant_ad->DetachFromOwner(true);
}

void TestReleaseReentersDetach()
{
    printf("TestReleaseReentersDetach\n");
    g_release_calls = 0;
    TTWSConnAdapter* ad = MakeAdapter(ReentrantRelease, nullptr);
    g_reentrant_ad      = ad;
    ad->ReleaseOnce();              
    CHECK(g_release_calls == 1);
    g_reentrant_ad = nullptr;
}

} 

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
