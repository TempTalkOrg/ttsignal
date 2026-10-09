///////////////////////////////////////////////////////////////////////////////
// file : LinuxNetlinkMonitor.cpp
// author : anto
//
// Linux implementation of INetworkPathMonitor backed by AF_NETLINK +
// NETLINK_ROUTE. We watch RTNLGRP_LINK / RTNLGRP_IPV4_ROUTE /
// RTNLGRP_IPV6_ROUTE for any change to the default route, then re-resolve
// the current default-route's outgoing interface index (RTM_GETROUTE on
// 0.0.0.0) and fire the SMPConnector callback only when the ifIndex
// actually flipped.
//
// Quirks:
//   - Linux netlink is noisy. DHCP renew, systemd-networkd restart, and
//     even temporary IPv6 RA refreshes spam events; we de-dupe on ifIndex
//     and add a 150ms debounce so a burst doesn't trigger several
//     UDPSender restarts in a row.
//   - We exclude loopback and well-known virtual interfaces (lo, docker*,
//     veth*, br-*, virbr*, vmnet*, tailscale*, tun*, tap*) by name. They
//     can show up as the "best" default route briefly during container
//     startup, and bouncing the QUIC connection onto them is never what
//     we want.
//   - The reader thread is owned by the monitor handle; tt_netmon_stop
//     wakes it via a short pipe + join, so the implementation is fully
//     self-contained.
///////////////////////////////////////////////////////////////////////////////

#include "LinuxNetlinkMonitor.h"
#include "LinuxPhysicalIface.h"

#if !defined(__linux__)
#  error "LinuxNetlinkMonitor is Linux-only."
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Monitor {
    TTPathChangeCallback callback   = nullptr;
    void*                userdata   = nullptr;
    int                  netlink_fd = -1;
    int                  wake_fd[2] = {-1, -1};   // [0]=read, [1]=write
    std::thread          worker;
    std::atomic<bool>    stop_requested { false };
    // Fingerprint of (ifIndex, sorted IP addrs on that ifName). ifIndex
    // alone is not enough on desktop Linux: when the user roams between two
    // SSIDs the active interface stays "wlan0" so RTA_OIF reports the same
    // index, but the local IPv4/IPv6 (and the gateway) absolutely change,
    // which means the QUIC 5-tuple is stale and we *must* restart the UDP
    // socket. Folding the per-interface IP set into the dedup key catches
    // that case while still ignoring DHCP refresh / RA churn that doesn't
    // actually move us off the current network.
    std::mutex           sig_mutex;
    std::string          last_signature;          // guarded by sig_mutex

    // 生效策略。由 tt_netmon_start 从 options->vpnPolicy 与 options->bypassVpn
    // 合并得出，必须在 worker 线程启动前赋值，之后不再变更。
    TTVpnPolicy          policy = TT_VPN_POLICY_PREFER_PHYSICAL;
};

// 原先这里有一个 IsVirtualIface 名字前缀黑名单，且无条件生效。判定逻辑已
// 整体迁移到 LinuxPhysicalIface.cpp：主依据换成
// /sys/class/net/<name>/device 是否存在（用户可以把物理网卡改名叫 tun0，
// 名字判断不可靠），同一份黑名单降级为 sysfs 不可读时的兜底。
// 是否启用过滤现在由 TTVpnPolicy 决定，见 QueryDefaultIfIndex。

// Issue a single RTM_GETROUTE for the unspecified destination (0.0.0.0/0
// or ::/0) and walk the multipart reply looking for an RTA_OIF attribute.
// Returns 0 if nothing usable was found. Family is AF_INET unless we want
// IPv6.
//
// The whole query is bounded to ~1s wall-clock. WSL2's lightweight Linux
// kernel has at least one reproducible hang on multipart NETLINK_ROUTE
// replies — the kernel never emits NLMSG_DONE for RTM_GETROUTE with
// dst_len=0, so a plain blocking recv() loop sleeps forever. This used
// to wedge tt_netmon_start's synchronous "fire initial callback" path
// and therefore wedged ttsignal.createConnector() on every WSL2
// invocation. SOCK_NONBLOCK + poll(timeout) keeps that bounded; if the
// query times out we just return 0 and let the netlink event reader
// pick up the route on the next genuine RTM_NEWROUTE / RTM_DELROUTE.
static int64_t QueryDefaultIfIndex(int family, TTVpnPolicy policy)
{
    int sock = socket(AF_NETLINK,
                      SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK,
                      NETLINK_ROUTE);
    if (sock < 0) return 0;

    struct {
        struct nlmsghdr nlh;
        struct rtmsg    rt;
        char            buf[256];
    } req = {};
    req.nlh.nlmsg_len   = NLMSG_LENGTH(sizeof(struct rtmsg));
    req.nlh.nlmsg_type  = RTM_GETROUTE;
    req.nlh.nlmsg_flags = NLM_F_REQUEST;
    req.nlh.nlmsg_seq   = 1;
    req.rt.rtm_family   = family;
    req.rt.rtm_dst_len  = 0; // default route

    if (send(sock, &req, req.nlh.nlmsg_len, 0) < 0) {
        close(sock);
        return 0;
    }

    char buf[8192];
    int64_t found = 0;
    // prefer 档专用：第一块被过滤掉的虚拟网卡默认路由。找不到任何物理
    // 网卡时回落到它——否则纯 VPN 机器会拿到 0，socket 完全不绑定，
    // 反而白白把包交给内核路由送进隧道。force 档不用它。
    int64_t fallback = 0;
    bool timed_out = false;

    // Wall-clock budget. Each poll() iteration is at most the remainder
    // of the 1s budget; if recv() reports EAGAIN we wait again on poll
    // until the budget is exhausted.
    auto deadline = std::chrono::steady_clock::now()
                    + std::chrono::milliseconds(1000);

    while (true) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) { timed_out = true; break; }
        int budget_ms = (int)std::chrono::duration_cast<
            std::chrono::milliseconds>(deadline - now).count();
        if (budget_ms <= 0) { timed_out = true; break; }

        struct pollfd pfd = {};
        pfd.fd     = sock;
        pfd.events = POLLIN;
        int pr = poll(&pfd, 1, budget_ms);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) { timed_out = true; break; }
        if (!(pfd.revents & POLLIN)) {
            // POLLERR / POLLHUP — treat as "kernel won't reply, give up".
            break;
        }

        ssize_t len = recv(sock, buf, sizeof(buf), MSG_DONTWAIT);
        if (len <= 0) {
            if (len < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                continue;
            break;
        }

        for (struct nlmsghdr* nh = (struct nlmsghdr*)buf;
             NLMSG_OK(nh, (size_t)len);
             nh = NLMSG_NEXT(nh, len))
        {
            if (nh->nlmsg_type == NLMSG_DONE)  goto done;
            if (nh->nlmsg_type == NLMSG_ERROR) goto done;
            if (nh->nlmsg_type != RTM_NEWROUTE) continue;

            struct rtmsg* rt = (struct rtmsg*)NLMSG_DATA(nh);
            if (rt->rtm_dst_len != 0) continue; // not a default route
            // Skip cached / cloned / link routes.
            if (rt->rtm_table != RT_TABLE_MAIN &&
                rt->rtm_table != RT_TABLE_DEFAULT) continue;

            int rtl = RTM_PAYLOAD(nh);
            for (struct rtattr* attr = RTM_RTA(rt);
                 RTA_OK(attr, rtl);
                 attr = RTA_NEXT(attr, rtl))
            {
                if (attr->rta_type == RTA_OIF) {
                    int oif = *(int*)RTA_DATA(attr);
                    char name[IF_NAMESIZE] = {0};
                    if_indextoname(oif, name);
                    // os 档不过滤：内核选出的默认路由赢家是谁就是谁，
                    // 含 tun / wireguard / tailscale。
                    // prefer / force 两档跳过虚拟网卡，继续在后续的
                    // RTM_NEWROUTE 里找物理网卡的默认路由。
                    if (policy != TT_VPN_POLICY_OS &&
                        !tt_linux_iface_is_physical(name)) {
                        if (fallback == 0) fallback = (int64_t)oif;
                        continue;
                    }
                    found = (int64_t)oif;
                    goto done;
                }
            }
        }
        // If we got a partial batch with no NLMSG_DONE but already
        // resolved an RTA_OIF we'd have jumped to done. Otherwise loop
        // back and poll for more — the budget guards against hangs.
    }
done:
    close(sock);
    if (timed_out && found == 0) {
        // One-line diagnostic so operators on WSL2 / kernels with broken
        // NLMSG_DONE handling can correlate "no path-monitor callback
        // ever fired" with the right root cause. stderr is fine; the
        // monitor doesn't have a logger handle.
        fprintf(stderr,
            "[ttsignal] LinuxNetlinkMonitor: RTM_GETROUTE family=%d "
            "timed out after 1s (kernel did not send NLMSG_DONE); "
            "treating as 'no default route yet'. Common on WSL2.\n",
            family);
        fflush(stderr);
    }
    if (found == 0 && policy == TT_VPN_POLICY_PREFER_PHYSICAL) {
        // 只有 prefer 档回落到隧道。force 档宁可返回 0，让
        // SMPConnection::Connect 以 BC_R_NO_PHYSICAL_INTERFACE 明确失败。
        found = fallback;
    }
    return found;
}

// Walk getifaddrs() and collect every non-loopback IPv4/IPv6 address bound
// to `ifName`. Returns "" if the interface has no addresses (yet) — that's
// fine, the next netlink wake-up will retry. The result is sorted so the
// kernel's linked-list order doesn't perturb the fingerprint.
static std::string CollectInterfaceIPs(const char* ifName)
{
    if (!ifName || !ifName[0]) return std::string();

    struct ifaddrs* head = nullptr;
    if (getifaddrs(&head) != 0 || !head) return std::string();

    std::vector<std::string> addrs;
    for (struct ifaddrs* it = head; it != nullptr; it = it->ifa_next) {
        if (!it->ifa_name || strcmp(it->ifa_name, ifName) != 0) continue;
        if (!it->ifa_addr) continue;
        if ((it->ifa_flags & IFF_LOOPBACK) != 0) continue;

        char buf[INET6_ADDRSTRLEN] = {0};
        int family = it->ifa_addr->sa_family;
        if (family == AF_INET) {
            const struct sockaddr_in* sin =
                (const struct sockaddr_in*)it->ifa_addr;
            if (!inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf))) continue;
        } else if (family == AF_INET6) {
            const struct sockaddr_in6* sin6 =
                (const struct sockaddr_in6*)it->ifa_addr;
            if (!inet_ntop(AF_INET6, &sin6->sin6_addr, buf, sizeof(buf))) continue;
            // IPv6 link-local (fe80::/10) is derived from the MAC address
            // (EUI-64) and doesn't change across SSIDs, so it would mask a
            // real network change. Skip it; routable addresses (DHCPv6 /
            // SLAAC global, ULA) still drive the fingerprint.
            if (sin6->sin6_addr.s6_addr[0] == 0xfe &&
                (sin6->sin6_addr.s6_addr[1] & 0xc0) == 0x80) {
                continue;
            }
        } else {
            continue;
        }
        addrs.emplace_back(buf);
    }
    freeifaddrs(head);

    std::sort(addrs.begin(), addrs.end());
    std::string out;
    out.reserve(addrs.size() * 16);
    for (const auto& a : addrs) {
        if (!out.empty()) out.push_back(',');
        out.append(a);
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

static void ReaderLoop(Monitor* self)
{
    auto last_event_time = std::chrono::steady_clock::now()
                           - std::chrono::seconds(1);

    char buf[8192];
    while (!self->stop_requested.load(std::memory_order_acquire)) {
        struct pollfd pfd[2];
        pfd[0].fd     = self->netlink_fd;
        pfd[0].events = POLLIN;
        pfd[1].fd     = self->wake_fd[0];
        pfd[1].events = POLLIN;

        int n = poll(pfd, 2, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pfd[1].revents & POLLIN) {
            char drop[64];
            (void)read(self->wake_fd[0], drop, sizeof(drop));
            if (self->stop_requested.load(std::memory_order_acquire)) break;
        }
        if (pfd[0].revents & POLLIN) {
            // Drain. We don't actually care about the contents — any
            // event in these groups means "re-evaluate the default
            // route". This also avoids the alignment minefield of
            // walking netlink mid-stream.
            ssize_t got = recv(self->netlink_fd, buf, sizeof(buf),
                               MSG_DONTWAIT);
            (void)got;
            // Drain anything else queued.
            while (recv(self->netlink_fd, buf, sizeof(buf),
                        MSG_DONTWAIT) > 0) {}
        }

        // Debounce: if we got a burst, sleep briefly so RTM_GETROUTE
        // sees the settled state.
        auto now = std::chrono::steady_clock::now();
        if (now - last_event_time
            < std::chrono::milliseconds(150)) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(150));
        }
        last_event_time = std::chrono::steady_clock::now();

        int64_t idx = QueryDefaultIfIndex(AF_INET, self->policy);
        if (idx <= 0) idx = QueryDefaultIfIndex(AF_INET6, self->policy);
        if (idx <= 0) continue; // no default route right now

        char name[IF_NAMESIZE] = {0};
        if_indextoname((unsigned)idx, name);

        std::string ips = CollectInterfaceIPs(name);
        std::string sig = MakeSignature(idx, ips);

        {
            std::lock_guard<std::mutex> lock(self->sig_mutex);
            if (self->last_signature == sig) continue;
            self->last_signature = sig;
        }

        char desc[64];
        snprintf(desc, sizeof(desc), "default (%s)",
                 name[0] ? name : "?");
        self->callback(self->userdata, idx, desc);
    }
}

} // namespace

extern "C" {

TTNetworkMonitorRef tt_netmon_start(const TTNetworkMonitorOptions* options,
                                    TTPathChangeCallback cb,
                                    void* userdata)
{
    if (!cb) return nullptr;
    Monitor* self = new Monitor();
    self->callback = cb;
    self->userdata = userdata;
    // 生效策略由 vpnPolicy / bypassVpn 合并得出。
    //
    // ⚠️ 必须在 worker 线程启动之前赋值——WorkerLoop 会读 self->policy。
    //
    // Linux 在本次改动前的默认行为已经等价于 prefer-physical：
    // QueryDefaultIfIndex 里的 IsVirtualIface 是无条件生效的名字黑名单，
    // 找不到物理网卡的默认路由就返回 0。所以默认值不变；新增的能力是
    // os 档（不过滤，跟随内核选出的默认路由，含 tun/wireguard）。
    if (options == nullptr) {
        self->policy = tt_vpn_policy_platform_default();
    } else {
        self->policy = tt_vpn_policy_resolve(
            (TTVpnPolicy)options->vpnPolicy,
            /*hasBypassVpn=*/1, options->bypassVpn,
            tt_vpn_policy_platform_default(),
            /*outBothGiven=*/nullptr);
    }

    self->netlink_fd = socket(AF_NETLINK,
                              SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK,
                              NETLINK_ROUTE);
    if (self->netlink_fd < 0) {
        delete self;
        return nullptr;
    }
    struct sockaddr_nl sa = {};
    sa.nl_family = AF_NETLINK;
    sa.nl_groups = RTMGRP_LINK
                 | RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR
                 | RTMGRP_IPV4_ROUTE  | RTMGRP_IPV6_ROUTE;
    if (bind(self->netlink_fd, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
        close(self->netlink_fd);
        delete self;
        return nullptr;
    }
    if (pipe(self->wake_fd) < 0) {
        close(self->netlink_fd);
        delete self;
        return nullptr;
    }
    fcntl(self->wake_fd[0], F_SETFL,
          fcntl(self->wake_fd[0], F_GETFL, 0) | O_NONBLOCK);

    // Fire initial callback synchronously so the caller learns the
    // current default-route ifIndex before connect().
    int64_t init_ifx = QueryDefaultIfIndex(AF_INET, self->policy);
    if (init_ifx <= 0) init_ifx = QueryDefaultIfIndex(AF_INET6, self->policy);
    if (init_ifx > 0) {
        char name[IF_NAMESIZE] = {0};
        if_indextoname((unsigned)init_ifx, name);
        {
            std::lock_guard<std::mutex> lock(self->sig_mutex);
            self->last_signature =
                MakeSignature(init_ifx, CollectInterfaceIPs(name));
        }
        char desc[64];
        snprintf(desc, sizeof(desc), "default (%s)",
                 name[0] ? name : "?");
        cb(userdata, init_ifx, desc);
    }

    self->worker = std::thread(ReaderLoop, self);
    return (TTNetworkMonitorRef)self;
}

void tt_netmon_stop(TTNetworkMonitorRef ref)
{
    Monitor* self = (Monitor*)ref;
    if (!self) return;
    self->stop_requested.store(true, std::memory_order_release);
    if (self->wake_fd[1] >= 0) {
        char b = 'x';
        (void)write(self->wake_fd[1], &b, 1);
    }
    if (self->worker.joinable()) self->worker.join();
    if (self->netlink_fd >= 0) close(self->netlink_fd);
    if (self->wake_fd[0]  >= 0) close(self->wake_fd[0]);
    if (self->wake_fd[1]  >= 0) close(self->wake_fd[1]);
    delete self;
}

int64_t tt_netmon_query_default_ifindex_ex(int vpnPolicy)
{
    TTVpnPolicy policy = (TTVpnPolicy)vpnPolicy;
    if (policy != TT_VPN_POLICY_OS &&
        policy != TT_VPN_POLICY_PREFER_PHYSICAL &&
        policy != TT_VPN_POLICY_FORCE_PHYSICAL) {
        policy = tt_vpn_policy_platform_default();
    }
    int64_t idx = QueryDefaultIfIndex(AF_INET, policy);
    if (idx <= 0) idx = QueryDefaultIfIndex(AF_INET6, policy);
    return idx;
}

int64_t tt_netmon_query_default_ifindex(void)
{
    return tt_netmon_query_default_ifindex_ex(
        (int)tt_vpn_policy_platform_default());
}

} // extern "C"
