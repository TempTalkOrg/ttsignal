///////////////////////////////////////////////////////////////////////////////
// file : WinIpChangeMonitor.cpp
// author : anto
//
// Windows implementation of INetworkPathMonitor backed by
// NotifyIpInterfaceChange (iphlpapi.dll). Whenever any IP interface state
// changes we re-resolve the "best" outgoing interface via
// GetBestInterfaceEx and only fire the SMP callback when the index
// actually flipped. Equivalent to the netlink-driven Linux flow.
//
// Concurrency:
//   NotifyIpInterfaceChange invokes its callback on an OS worker thread
//   under arbitrary contention, so we marshal the work onto our own
//   detached debounce thread, exactly like the netlink reader. This way
//   the callback into SMPConnector is single-threaded and we don't have
//   to worry about overlapping restarts.
//
// MinGW notes:
//   NotifyIpInterfaceChange + GetBestInterfaceEx have lived in
//   iphlpapi.h since Vista and are present on the toolchain we use
//   (mingw-w64 ≥ 10). Link with -liphlpapi -lws2_32. If the symbol is
//   missing on a paleolithic toolchain, build will fail at link time
//   and the operator can fall back to disabling the monitor (see
//   `disableAutoRestart` config option).
///////////////////////////////////////////////////////////////////////////////

#include "WinIpChangeMonitor.h"

#if !defined(_WIN32)
#  error "WinIpChangeMonitor is Windows-only."
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601 // Windows 7+
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Monitor {
    TTPathChangeCallback     callback   = nullptr;
    void*                    userdata   = nullptr;
    HANDLE                   notify_h   = nullptr;
    std::atomic<bool>        stop       { false };

    // Fingerprint of (ifIndex, sorted IP addrs on that ifIndex). ifIndex
    // alone is wrong on Windows desktops: when the user roams between two
    // SSIDs the active Wi-Fi adapter stays the same so GetBestInterfaceEx
    // keeps returning the same NET_IFINDEX, but the local IPv4/IPv6 (and
    // the gateway) absolutely change, which means the QUIC 5-tuple is
    // stale and we *must* restart the UDP socket. Folding the per-interface
    // unicast IP set into the dedup key catches that case while still
    // ignoring DHCP refresh / RA churn that doesn't actually move us off
    // the current network.
    std::mutex               sig_mtx;
    std::string              last_sig;          // guarded by sig_mtx

    // Debounce thread: callback signals it via cv, it sleeps a bit then
    // re-resolves the best interface.
    std::thread              worker;
    std::mutex               mtx;
    std::condition_variable  cv;
    bool                     pending    { false };
};

// GetBestInterfaceEx wants a sockaddr destination. Using INADDR_ANY (0.0.0.0)
// asks Windows for the best interface to reach an unspecified IPv4 dest,
// which is effectively "give me the default-route interface". We try IPv4
// first; if that fails we try the IPv6 unspecified address.
static int64_t QueryBestIfIndex()
{
    struct sockaddr_in dst4 = {};
    dst4.sin_family = AF_INET;
    dst4.sin_addr.s_addr = INADDR_ANY;
    DWORD ifx = 0;
    if (GetBestInterfaceEx((struct sockaddr*)&dst4, &ifx) == NO_ERROR
        && ifx != 0) {
        return (int64_t)ifx;
    }

    struct sockaddr_in6 dst6 = {};
    dst6.sin6_family = AF_INET6;
    if (GetBestInterfaceEx((struct sockaddr*)&dst6, &ifx) == NO_ERROR
        && ifx != 0) {
        return (int64_t)ifx;
    }
    return 0;
}

// Walk GetUnicastIpAddressTable() and collect every non-loopback IPv4/IPv6
// address bound to `ifIndex`. Returns "" if nothing was found — that's fine,
// the next NotifyIpInterfaceChange wake will retry. The result is sorted so
// the kernel's enumeration order doesn't perturb the fingerprint.
static std::string CollectInterfaceIPs(int64_t ifIndex)
{
    if (ifIndex <= 0) return std::string();

    PMIB_UNICASTIPADDRESS_TABLE table = nullptr;
    if (GetUnicastIpAddressTable(AF_UNSPEC, &table) != NO_ERROR || !table) {
        return std::string();
    }

    std::vector<std::string> addrs;
    for (ULONG i = 0; i < table->NumEntries; ++i) {
        const MIB_UNICASTIPADDRESS_ROW& row = table->Table[i];
        if ((int64_t)row.InterfaceIndex != ifIndex) continue;

        char buf[INET6_ADDRSTRLEN] = {0};
        const SOCKADDR_INET& a = row.Address;
        if (a.si_family == AF_INET) {
            if (!inet_ntop(AF_INET, (void*)&a.Ipv4.sin_addr,
                           buf, sizeof(buf))) continue;
        } else if (a.si_family == AF_INET6) {
            if (!inet_ntop(AF_INET6, (void*)&a.Ipv6.sin6_addr,
                           buf, sizeof(buf))) continue;
            // IPv6 link-local (fe80::/10) is derived from the MAC address
            // and doesn't change across SSIDs. Skip it; routable addresses
            // (DHCPv6 / SLAAC global, ULA) still drive the fingerprint.
            const UCHAR* b = a.Ipv6.sin6_addr.s6_addr;
            if (b[0] == 0xfe && (b[1] & 0xc0) == 0x80) continue;
        } else {
            continue;
        }
        addrs.emplace_back(buf);
    }
    FreeMibTable(table);

    std::sort(addrs.begin(), addrs.end());
    std::string out;
    out.reserve(addrs.size() * 16);
    for (const auto& s : addrs) {
        if (!out.empty()) out.push_back(',');
        out.append(s);
    }
    return out;
}

static std::string MakeSignature(int64_t ifIndex, const std::string& ips)
{
    char head[32];
    snprintf(head, sizeof(head), "%lld|", (long long)ifIndex);
    std::string sig = head;
    sig.append(ips);
    return sig;
}

static void DescribeIfIndex(int64_t ifx, char* out, size_t cap)
{
    // GetIfEntry2 gives us the "Alias" (e.g. "Wi-Fi", "Ethernet"); fall
    // back to the numeric index if anything fails.
    MIB_IF_ROW2 row = {};
    row.InterfaceIndex = (NET_IFINDEX)ifx;
    if (GetIfEntry2(&row) == NO_ERROR) {
        // Alias is wide; convert to ASCII (best-effort, log-only).
        char alias[128] = {0};
        WideCharToMultiByte(CP_UTF8, 0, row.Alias, -1, alias,
                            sizeof(alias), nullptr, nullptr);
        snprintf(out, cap, "best (%s, ifx=%lld)",
                 alias[0] ? alias : "?", (long long)ifx);
        return;
    }
    snprintf(out, cap, "best (ifx=%lld)", (long long)ifx);
}

// Worker thread: debounce + dispatch.
static void WorkerLoop(Monitor* self)
{
    for (;;) {
        std::unique_lock<std::mutex> lk(self->mtx);
        self->cv.wait(lk, [self] {
            return self->stop.load() || self->pending;
        });
        if (self->stop.load()) return;
        self->pending = false;
        lk.unlock();

        // Sleep briefly so we coalesce a burst (DHCP renew etc.).
        std::this_thread::sleep_for(std::chrono::milliseconds(150));

        // Drain any extra signals that arrived during the sleep.
        {
            std::lock_guard<std::mutex> g(self->mtx);
            self->pending = false;
        }
        if (self->stop.load()) return;

        int64_t ifx = QueryBestIfIndex();
        if (ifx <= 0) continue;

        std::string sig = MakeSignature(ifx, CollectInterfaceIPs(ifx));
        {
            std::lock_guard<std::mutex> lock(self->sig_mtx);
            if (self->last_sig == sig) continue;
            self->last_sig = sig;
        }

        char desc[160];
        DescribeIfIndex(ifx, desc, sizeof(desc));
        self->callback(self->userdata, ifx, desc);
    }
}

// NotifyIpInterfaceChange callback. Runs on an OS worker thread; do as
// little as possible here.
static VOID WINAPI OnIpIfChange(PVOID context,
                                PMIB_IPINTERFACE_ROW /*row*/,
                                MIB_NOTIFICATION_TYPE /*type*/)
{
    Monitor* self = (Monitor*)context;
    if (!self || self->stop.load()) return;

    {
        std::lock_guard<std::mutex> g(self->mtx);
        self->pending = true;
    }
    self->cv.notify_one();
}

} // namespace

extern "C" {

TTNetworkMonitorRef tt_netmon_start(TTPathChangeCallback cb, void* userdata)
{
    if (!cb) return nullptr;
    Monitor* self = new Monitor();
    self->callback = cb;
    self->userdata = userdata;
    self->worker   = std::thread(WorkerLoop, self);

    // Initial fire so the caller sees the current default-route ifx
    // before connect().
    int64_t init = QueryBestIfIndex();
    if (init > 0) {
        {
            std::lock_guard<std::mutex> lock(self->sig_mtx);
            self->last_sig = MakeSignature(init, CollectInterfaceIPs(init));
        }
        char desc[160];
        DescribeIfIndex(init, desc, sizeof(desc));
        cb(userdata, init, desc);
    }

    DWORD r = NotifyIpInterfaceChange(AF_UNSPEC,
                                      (PIPINTERFACE_CHANGE_CALLBACK)
                                          OnIpIfChange,
                                      self,
                                      FALSE,           // initial notify
                                      &self->notify_h);
    if (r != NO_ERROR) {
        // Could not register; fall back to a no-op monitor (the worker
        // thread will sit on its cv until tt_netmon_stop kicks it).
        self->notify_h = nullptr;
    }
    return (TTNetworkMonitorRef)self;
}

void tt_netmon_stop(TTNetworkMonitorRef ref)
{
    Monitor* self = (Monitor*)ref;
    if (!self) return;
    self->stop.store(true, std::memory_order_release);
    if (self->notify_h) {
        CancelMibChangeNotify2(self->notify_h);
        self->notify_h = nullptr;
    }
    self->cv.notify_all();
    if (self->worker.joinable()) self->worker.join();
    delete self;
}

int64_t tt_netmon_query_default_ifindex(void)
{
    return QueryBestIfIndex();
}

} // extern "C"
