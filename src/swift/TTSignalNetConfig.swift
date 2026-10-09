///////////////////////////////////////////////////////////////////////////////
// file : TTSignalNetConfig.swift
// author : anto
//
// HTTP 与 WebSocket 两套连接器共享的网络配置。
//
// 为什么不复用 TTSignalConfig：那个是 SMP/QUIC 栈的配置，200+ 行里绝大多数字段
// （alpn / congestionControl / proxyUrl / maxStreams / ...）对这条 TCP 栈**完全
// 无效**。摆在同一个类型上，调用方无从判断哪些真的生效。Node 侧同理，
// createHttpConnector / createWsConnector 各有自己的一套配置。
///////////////////////////////////////////////////////////////////////////////

import Foundation
import TTSignalC

public struct TTSignalNetConfig {

    /// VPN / 虚拟网卡选择策略。`nil` = 平台默认（不向原生层写这个键）。
    ///
    /// `.forcePhysical` 是这条栈的核心用途：绕开 VPN / 虚拟网卡，用**用户真实
    /// IP** 出网。典型场景是请求接入点接口 —— 走 VPN 出口拿到的接入点会被调度
    /// 到 VPN 出口所在地域，是错的。
    ///
    /// 是否真的绑上了物理网卡，看 `TTSignalHttpResponse.boundIfIndex` /
    /// `TTSignalWSConnection.boundIfIndex`：非 0 就是真的绑了。
    public var vpnPolicy: TTSignalVPNPolicy? = nil

    /// 额外信任的 PEM 文本。空则用系统信任库。
    public var caCerts: String = ""

    /// base64 SPKI pin。给了就只比对 pin。
    public var spkiPin: String = ""

    /// 仅供自签调试，原生层会打 WARN。**别在生产用**。
    public var insecureSkipVerify: Bool = false

    /// 自建 DNS 查询用的服务器列表。空则用系统解析。
    public var dnsServers: String = ""

    /// 单台 DNS server 的超时。0 = 用原生默认（2000ms）。
    public var dnsTimeoutMs: UInt32 = 0

    /// 关闭时排空在途请求的上限。0 = 用原生默认（30000ms）。
    public var drainTimeoutMs: UInt32 = 0

    /// BC 日志级别（1 = DEBUG ... 5 = FATAL）。0 = 用原生默认。
    public var logLevel: Int32 = 0

    public init() {}
}

///////////////////////////////////////////////////////////////////////////////
// C 字符串搬运
//
// 把 Swift String 变成在整个 C 调用期间都有效的 const char*。用 strdup + 显式
// free，而不是 withUnsafeBufferPointer —— 后者的指针只在闭包内有保证，逃出闭包
// 就是未定义行为。
///////////////////////////////////////////////////////////////////////////////

final class CStringBag {
    private var owned: [UnsafeMutablePointer<CChar>] = []

    /// 空串返回 nil —— C 侧一律把 NULL 与 "" 同等对待（不写那个配置键）。
    func dup(_ s: String) -> UnsafePointer<CChar>? {
        guard !s.isEmpty else { return nil }
        guard let p = strdup(s) else { return nil }
        owned.append(p)
        return UnsafePointer(p)
    }

    deinit {
        for p in owned { free(p) }
    }
}

///////////////////////////////////////////////////////////////////////////////
// End of file : TTSignalNetConfig.swift
///////////////////////////////////////////////////////////////////////////////
