///////////////////////////////////////////////////////////////////////////////
// file   : VpnPolicy.h
// author : anto
//
// VPN / 虚拟网卡选择策略。三态取代原先的 bypassVpn 布尔开关：
//
//   os              — 完全跟随系统路由，允许 QUIC 流量走 VPN 隧道
//   prefer-physical — 优先物理网卡（wifi / wired / cellular）；启动时找不到
//                     物理网卡可回落到隧道，运行中拒绝回落（保持现有 socket）
//   force-physical  — 只接受物理网卡。任何阶段都不回落，且禁止 UDPSender 撤销
//                     已有的网卡绑定。找不到物理网卡时连接直接失败，业务据此
//                     决定是否降级重连
//
// 除了 struct sockaddr 需要的系统头，本文件不依赖任何平台 API，可以被任意 TU
// include，也可以单独编译进单测。唯一的条件编译在
// tt_vpn_policy_platform_default() 里。
///////////////////////////////////////////////////////////////////////////////

#ifndef TT_VPN_POLICY_H
#define TT_VPN_POLICY_H

#include <stddef.h>
#include <stdint.h>

// tt_vpn_pin_recheck() 的契约是用 TT_ROUTE_IFINDEX_UNKNOWN / _NO_ROUTE 这两个
// 哨兵值表达的，所以直接把它们带进来，省得每个调用方各自 include。
// NetworkRouteLookup.h 只有常量与一条函数声明，不引入任何链接依赖。
#include "NetworkRouteLookup.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

// 数值编码是跨 ABI 契约的一部分（TTNetworkMonitorOptions::vpnPolicy 和
// TTConfig::vpnPolicy 都以 int 传递同一组取值），不要改动。
typedef enum {
    TT_VPN_POLICY_UNSET           = -1,  // 未设置，由调用方决定回落
    TT_VPN_POLICY_OS              = 0,
    TT_VPN_POLICY_PREFER_PHYSICAL = 1,
    TT_VPN_POLICY_FORCE_PHYSICAL  = 2,
} TTVpnPolicy;

// 当前平台在调用方什么都没配时应该采用的策略。
//   iOS / iPadOS / tvOS / watchOS（不含 Mac Catalyst） -> TT_VPN_POLICY_OS
//   macOS / Windows / Linux / 其它                     -> TT_VPN_POLICY_PREFER_PHYSICAL
//
// iOS 单独定为 os，是因为 iOS 上安装 per-app VPN 的用户通常就是希望流量走
// VPN；而 macOS/Linux 的既有默认行为本来就等价于 prefer-physical。Windows
// 的既有行为其实是 os，这里有意对齐为 prefer-physical，属于本次唯一有意引入
// 的默认行为变更（详见 spec）。
TTVpnPolicy tt_vpn_policy_platform_default(void);

// 解析配置里的字符串取值。大小写敏感，只认 "os" / "prefer-physical" /
// "force-physical"。NULL、空串、无法识别的取值一律返回 TT_VPN_POLICY_UNSET，
// 由调用方决定是否打告警日志。
TTVpnPolicy tt_vpn_policy_from_string(const char* s);

// 供日志使用。任何取值（含越界）都返回非 NULL 的静态字符串。
const char* tt_vpn_policy_to_string(TTVpnPolicy p);

// 合并新键 vpnPolicy 与旧键 bypassVpn，得出最终生效的策略。
//
//   explicitPolicy  : tt_vpn_policy_from_string 的结果，未配置时传 UNSET
//   hasBypassVpn    : 配置里是否出现过 bypassVpn 键（非 0 表示出现过）
//   bypassVpn       : 该键的布尔值。hasBypassVpn 为 0 时本参数被忽略
//   platformDefault : 两个键都没给时的回落值，通常传
//                     tt_vpn_policy_platform_default()。做成参数是为了让单测
//                     能在任意宿主平台上验证回落逻辑
//   outBothGiven    : 可传 NULL。两个键同时出现时置 1，否则置 0。调用方应据
//                     此打一条 _WARN_ 提示 bypassVpn 被忽略
//
// 优先级：explicitPolicy > bypassVpn 兼容映射 > platformDefault。
// 兼容映射：bypassVpn=false -> os，bypassVpn=true -> prefer-physical。
// 返回值永远是 OS / PREFER_PHYSICAL / FORCE_PHYSICAL 三者之一，不会是 UNSET。
TTVpnPolicy tt_vpn_policy_resolve(TTVpnPolicy explicitPolicy,
                                  int hasBypassVpn,
                                  int bypassVpn,
                                  TTVpnPolicy platformDefault,
                                  int* outBothGiven);

///////////////////////////////////////////////////////////////////////////////
// 对端地址分类 + 绑定复核决策
//
// 这两个函数是从 UDPSender::Connect() 里抽出来的纯逻辑，抽出来的唯一目的是让
// 它可被单测穷举。它们不碰任何系统调用，也不依赖 UDPSender / TcpChannel。
///////////////////////////////////////////////////////////////////////////////

// tt_peer_address_class() 的返回位。0 表示"全局可路由的普通公网地址"。
//
// 为什么需要这个分类：判断"把包硬绑在物理网卡上会不会打进黑洞"，靠路由表是
// 答不出来的。物理网卡只要有默认路由，任何目的地址在它的 scoped 路由表里都
// "有路可走"——但如果目的地址本身在公网上不可达（代理的 fake-IP、企业内网的
// RFC1918 地址），包发出去就是静默丢弃。地址段是这里唯一可靠的判据。
#define TT_PEER_CLASS_GLOBAL    ((uint32_t)0u)
// TUN 模式代理的 fake-IP 段。路由只存在于代理适配器内部，强行从物理网卡发出
// 去必然黑洞。198.18.0.0/15（RFC 2544，Clash / Mihomo / Surge≥4 / sing-box /
// Xray / Loon / Shadowrocket 的出厂默认）与 28.0.0.0/8（Surge 旧版默认）。
#define TT_PEER_CLASS_FAKE_IP   ((uint32_t)0x1u)
// 其它非全局可路由地址：RFC1918 私网、CGNAT、链路本地、环回、文档/测试段、
// 组播、保留段，以及 IPv6 的 ULA / 链路本地 / 环回 / 组播 / 文档段。典型场景
// 是"服务端部署在企业内网，只能经 VPN 到达"。
#define TT_PEER_CLASS_PRIVATE   ((uint32_t)0x2u)

// 判断对端地址属于哪一类。dst 为 NULL、长度不足、地址族不是 AF_INET/AF_INET6
// 时一律返回 TT_PEER_CLASS_GLOBAL（"不知道"就按最保守的"普通公网地址"处理，
// 不凭空制造黑洞判定）。IPv4-mapped IPv6（::ffff:a.b.c.d）按内嵌的 IPv4 分类。
uint32_t tt_peer_address_class(const struct sockaddr* dst, size_t dst_len);

// 复核结论。
typedef enum {
    TT_PIN_VERDICT_KEEP  = 0,  // 保持网卡绑定
    TT_PIN_VERDICT_UNPIN = 1,  // 解除绑定，回落系统默认路由
} TTPinVerdict;

// 复核理由。KEEP 时恒为 TT_PIN_REASON_OK；UNPIN 时说明是哪一条判据命中，
// 调用方据此选日志文案。
typedef enum {
    TT_PIN_REASON_OK = 0,
    // scoped 路由表说：绑定的网卡把包判给了别的网卡。
    TT_PIN_REASON_SCOPED_MISMATCH,
    // scoped 路由表说：绑定的网卡压根没有到对端的路由（确定性不可达）。
    TT_PIN_REASON_SCOPED_NO_ROUTE,
    // 对端地址非全局可路由，且全局路由表把它判给了别人（或查不出来）——
    // 从物理网卡硬发出去会静默黑洞。
    TT_PIN_REASON_BLACKHOLE_RISK,
    // 全局路由表与绑定不一致。仅 os 档使用：该档的语义就是"跟随系统"。
    TT_PIN_REASON_GLOBAL_MISMATCH,
} TTPinRecheckReason;

/**
 * @brief 已经钉住网卡的 socket，在发第一个包之前复核这个钉子该不该留。
 *
 * ⚠️ 两次路由查询问的是**不同的问题**，不要合并成一次：
 *
 *   * scopedIfIndex（tt_route_lookup_ifindex(..., scope=boundIfIndex)）
 *     回答"这条已经硬绑在 boundIfIndex 上的 socket，包会从哪出去"。这是
 *     **绑定有效性**的唯一正确判据：macOS 的 IP_BOUND_IF 是硬绑定，内核会在
 *     该网卡的 scoped 路由表里重做 FIB 查询，谁占着全局默认路由与它无关。
 *     拿全局答案去校验硬绑定 = VPN 抢默认路由时 100% 假阴性。
 *
 *   * globalIfIndex（scope=0）
 *     回答"内核本来想把这个对端交给谁"。它**只**用于黑洞启发式，不参与绑定
 *     有效性判定。
 *
 * 为什么 scoped 查询单独不够——这是本函数存在的核心原因，实测数据（macOS
 * 15.6.1 + GlobalProtect 全隧道，en0=14 / utun4=22）：
 *
 *     route -n get            198.18.0.5  -> utun4
 *     route -n get -ifscope en0 198.18.0.5 -> en0
 *     route -n get            10.10.0.1   -> utun4
 *     route -n get -ifscope en0 10.10.0.1  -> en0
 *
 * macOS 的 scoped 查询会**忽略挂在别的网卡上的非 scoped 专用路由**。也就是说
 * 只要被绑网卡有默认路由，scoped 查询几乎永远答"被绑网卡自己"。若只看 scoped
 * 结果，fake-IP 与内网地址的黑洞保护会被彻底缴械——包发出去静默丢弃，QUIC 这
 * 条路径没有任何反应式补救（IP_BOUND_IF 下 connect()/send() 都返回成功）。
 * 所以黑洞判定必须另走一条判据：**地址段 + 全局路由表**。
 *
 * 反过来，对普通公网地址就不需要这条判据：企业 VPN 抢走默认路由后，把包从
 * en0 硬发给一个真实公网 IP 是**能到的**（服务端看到真实 IP，正是
 * prefer-physical 想要的结果），不是黑洞。fake-IP / 私网地址才是真黑洞。
 * 这就是 peerClass 作为判别器的理由。
 *
 * 分档语义（差异是刻意的，不要"统一"掉）：
 *
 *   * force-physical — 永不解绑。业务显式声明"必须拿到真实 IP"，此时保持绑定、
 *     让连接失败才是正确行为；静默回落会把"有时拿到真实 IP、有时拿不到"变成
 *     不可复现的线上问题。恒返回 KEEP，诊断日志由调用方按需打。
 *   * prefer-physical — 尽力走物理网卡。绑定有效性看 scoped，黑洞保护看
 *     地址段 + 全局表。
 *   * os / unset — 语义就是"跟随系统路由"，所以全局表说走别处就跟着走。这是
 *     已上线的既有行为，逐位保持不变。
 *
 * @param policy         生效的策略。UNSET 与越界值按 os 处理（保守：等同既有行为）。
 * @param boundIfIndex   socket 实际钉住的 ifIndex。0 表示没钉，直接 KEEP。
 * @param scopedIfIndex  tt_route_lookup_ifindex(..., scope=boundIfIndex) 的结果。
 * @param globalIfIndex  tt_route_lookup_ifindex(..., scope=0) 的结果。
 * @param peerClass      tt_peer_address_class() 的结果。调用方可以传
 *                       TT_PEER_CLASS_GLOBAL 来关闭地址段启发式（非 Apple
 *                       平台就是这么做的，用于保持那些平台的既有行为）。
 * @param outReason      可传 NULL。命中的判据。
 */
TTPinVerdict tt_vpn_pin_recheck(TTVpnPolicy policy,
                                uint32_t boundIfIndex,
                                uint32_t scopedIfIndex,
                                uint32_t globalIfIndex,
                                uint32_t peerClass,
                                TTPinRecheckReason* outReason);

// 供日志使用。任何取值都返回非 NULL 的静态字符串。
const char* tt_vpn_pin_reason_to_string(TTPinRecheckReason r);

#ifdef __cplusplus
}
#endif

#endif // TT_VPN_POLICY_H

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
