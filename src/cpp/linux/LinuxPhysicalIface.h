///////////////////////////////////////////////////////////////////////////////
// file   : LinuxPhysicalIface.h
// author : anto
//
// 判断一块 Linux 网卡是不是"真实物理网卡"。用于 prefer-physical /
// force-physical 策略下过滤掉 VPN 隧道与各种虚拟网桥。
//
// 主依据是 /sys/class/net/<name>/device 这个符号链接——只有背后真的挂着硬件
// 设备（PCI / USB / SDIO ...）的网卡才有它。tun/tap/bridge/veth/wireguard 都
// 是纯软件设备，没有。这比原先的名字前缀黑名单可靠得多：用户完全可以把物理
// 网卡改名叫 tun0，也可以把 WireGuard 接口改名叫 eth9。
//
// 名字黑名单保留为兜底，只在 sysfs 整体不可读时才生效（受限容器、挂了
// hidepid 的宿主、以及本文件的单测在 macOS 上跑的时候）。
///////////////////////////////////////////////////////////////////////////////

#ifndef TT_LINUX_PHYSICAL_IFACE_H
#define TT_LINUX_PHYSICAL_IFACE_H

#ifdef __cplusplus
extern "C" {
#endif

// 返回 1 表示物理网卡，0 表示虚拟网卡或无法判定。
// 内部走默认根路径 /sys/class/net。
int tt_linux_iface_is_physical(const char* ifName);

// 同上，但允许指定 /sys/class/net 的替代根路径，供单测注入假目录树。
// sysClassNetRoot 传 NULL 等同于调用 tt_linux_iface_is_physical。
//
// 判定顺序：
//   1. ifName 非法（NULL / 空 / 含 '/' 或以 '.' 开头）-> 0
//   2. <root> 本身不可读 -> 回落名字黑名单
//   3. <root>/<ifName>/device 存在 -> 1
//   4. <root>/<ifName> 存在但没有 device/ -> 0（确实是软件设备）
//   5. <root>/<ifName> 也不存在 -> 回落名字黑名单
int tt_linux_iface_is_physical_at(const char* ifName,
                                  const char* sysClassNetRoot);

#ifdef __cplusplus
}
#endif

#endif // TT_LINUX_PHYSICAL_IFACE_H

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
