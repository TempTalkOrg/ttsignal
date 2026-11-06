
























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
    int                  wake_fd[2] = {-1, -1};   
    std::thread          worker;
    std::atomic<bool>    stop_requested { false };
    
    
    
    
    
    
    
    
    std::mutex           sig_mutex;
    std::string          last_signature;          

    
    
    TTVpnPolicy          policy = TT_VPN_POLICY_PREFER_PHYSICAL;
};





















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
    req.rt.rtm_dst_len  = 0; 

    if (send(sock, &req, req.nlh.nlmsg_len, 0) < 0) {
        close(sock);
        return 0;
    }

    char buf[8192];
    int64_t found = 0;
    
    
    
    int64_t fallback = 0;
    bool timed_out = false;

    
    
    
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
            if (rt->rtm_dst_len != 0) continue; 
            
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
        
        
        
    }
done:
    close(sock);
    if (timed_out && found == 0) {
        
        
        
        
        fprintf(stderr,
            "[ttsignal] LinuxNetlinkMonitor: RTM_GETROUTE family=%d "
            "timed out after 1s (kernel did not send NLMSG_DONE); "
            "treating as 'no default route yet'. Common on WSL2.\n",
            family);
        fflush(stderr);
    }
    if (found == 0 && policy == TT_VPN_POLICY_PREFER_PHYSICAL) {
        
        
        found = fallback;
    }
    return found;
}





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
            
            
            
            
            ssize_t got = recv(self->netlink_fd, buf, sizeof(buf),
                               MSG_DONTWAIT);
            (void)got;
            
            while (recv(self->netlink_fd, buf, sizeof(buf),
                        MSG_DONTWAIT) > 0) {}
        }

        
        
        auto now = std::chrono::steady_clock::now();
        if (now - last_event_time
            < std::chrono::milliseconds(150)) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(150));
        }
        last_event_time = std::chrono::steady_clock::now();

        int64_t idx = QueryDefaultIfIndex(AF_INET, self->policy);
        if (idx <= 0) idx = QueryDefaultIfIndex(AF_INET6, self->policy);
        if (idx <= 0) continue; 

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

} 

extern "C" {

TTNetworkMonitorRef tt_netmon_start(const TTNetworkMonitorOptions* options,
                                    TTPathChangeCallback cb,
                                    void* userdata)
{
    if (!cb) return nullptr;
    Monitor* self = new Monitor();
    self->callback = cb;
    self->userdata = userdata;
    
    
    
    
    
    
    
    
    if (options == nullptr) {
        self->policy = tt_vpn_policy_platform_default();
    } else {
        self->policy = tt_vpn_policy_resolve(
            (TTVpnPolicy)options->vpnPolicy,
            1, options->bypassVpn,
            tt_vpn_policy_platform_default(),
            nullptr);
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

} 
