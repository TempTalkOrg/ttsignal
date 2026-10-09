///////////////////////////////////////////////////////////////////////////////
// file   : LinuxPhysicalIface.cpp
// author : anto
///////////////////////////////////////////////////////////////////////////////

#include "LinuxPhysicalIface.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

const char* const kDefaultSysClassNet = "/sys/class/net";

// 名字前缀黑名单。仅在 sysfs 不可读时兜底使用，条目沿用
// LinuxNetlinkMonitor.cpp 原有的 IsVirtualIface 列表。
bool NameLooksVirtual(const char* name)
{
    static const char* const kBlacklist[] = {
        "lo",        // loopback
        "docker",    // docker0, docker_gwbridge
        "br-",       // docker bridge networks
        "veth",      // virtual eth between container and host
        "virbr",     // libvirt
        "vmnet",     // VMware host-only
        "tailscale", // tailscale0
        "tun",       // tun0..N (OpenVPN/WireGuard)
        "tap",       // tap0..N
        "zt",        // zerotier
        "kube-",     // kube-bridge variants
        "cni",       // cni0
        "flannel",   // flannel.1
        NULL,
    };
    for (int i = 0; kBlacklist[i]; i++) {
        if (strncmp(name, kBlacklist[i], strlen(kBlacklist[i])) == 0) {
            return true;
        }
    }
    return false;
}

// 网卡名不能含路径分隔符，也不能以 '.' 开头——否则拼进 sysfs 路径会变成
// 目录穿越。内核本身也不允许这样的接口名。
bool NameIsSane(const char* name)
{
    if (name == NULL || name[0] == '\0') return false;
    if (name[0] == '.') return false;
    if (strchr(name, '/') != NULL) return false;
    return true;
}

bool PathExists(const char* path)
{
    struct stat st;
    // stat 而非 lstat：device 是符号链接，我们关心它指向的目标存不存在。
    return stat(path, &st) == 0;
}

} // namespace

extern "C" {

int tt_linux_iface_is_physical_at(const char* ifName,
                                  const char* sysClassNetRoot)
{
    if (!NameIsSane(ifName)) {
        return 0;
    }
    const char* root = (sysClassNetRoot && sysClassNetRoot[0])
                     ? sysClassNetRoot
                     : kDefaultSysClassNet;

    // sysfs 根都不可读，只能靠名字猜
    if (!PathExists(root)) {
        return NameLooksVirtual(ifName) ? 0 : 1;
    }

    char path[512];
    int n = snprintf(path, sizeof(path), "%s/%s/device", root, ifName);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        // 名字长到把缓冲区撑爆，不可能是正常网卡（IF_NAMESIZE 是 16）
        return 0;
    }
    if (PathExists(path)) {
        return 1;
    }

    // <root>/<ifName> 存在但没有 device/：确凿的软件设备
    n = snprintf(path, sizeof(path), "%s/%s", root, ifName);
    if (n > 0 && (size_t)n < sizeof(path) && PathExists(path)) {
        return 0;
    }

    // 网卡在 sysfs 里根本不存在（刚被删掉 / 名字来自过期的 netlink 事件），
    // 退回名字判断而不是武断地判成物理网卡。
    return NameLooksVirtual(ifName) ? 0 : 1;
}

int tt_linux_iface_is_physical(const char* ifName)
{
    return tt_linux_iface_is_physical_at(ifName, NULL);
}

} // extern "C"

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
