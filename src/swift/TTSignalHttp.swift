///////////////////////////////////////////////////////////////////////////////
// file : TTSignalHttp.swift
// author : anto
//
// HTTP / HTTPS 一次性请求。Swift mirror of src/cpp/apple/ios_http_bridge.mm 的
// tt_http_* C-ABI，能力面与 Node 侧 ttsignal.createHttpConnector 对齐。
//
// 与 TTSignalConnector（SMP/QUIC 信令）是**两条独立的栈**，不共用连接器。这条
// 走 TCP，用途是在 VPN / 虚拟网卡环境下用用户真实 IP 出网。
//
// ⚠️ 线程：completion 在原生工作线程上恢复 continuation，因此 `await` 之后的
// 代码会先在那条线程上跑一段。**别在那里做耗时的事**，也别直接碰 UI —— 需要
// 主线程就自己 hop（await MainActor.run { ... }）。Node 侧有同样的坑，
// src/js/index.js 的注释里专门写了一段。
///////////////////////////////////////////////////////////////////////////////

import Foundation
import TTSignalC

///////////////////////////////////////////////////////////////////////////////
// 配置
///////////////////////////////////////////////////////////////////////////////

public struct TTSignalHttpConfig {
    /// 与 WS 共享的那部分（vpnPolicy / caCerts / dns* / ...）。
    public var net = TTSignalNetConfig()

    /// 响应体上限。超过报 `BC_R_RESPONSE_TOO_LARGE`(73)。0 = 用原生默认（8 MiB）。
    public var maxResponseBytes: Int = 0

    public init() {}
}

///////////////////////////////////////////////////////////////////////////////
// 响应
///////////////////////////////////////////////////////////////////////////////

public struct TTSignalHttpResponse {
    public let status: Int32

    /// status line 的 reason-phrase，如 "Not Found"。
    ///
    /// ⚠️ **原始网络字节，未做净化** —— 可能含控制字符（包括 ESC，足以往终端
    /// 注入 ANSI 转义序列）。直接打到终端 / 写进日志 / 塞进 HTML 之前自己过滤。
    public let reason: String

    /// 头名**统一小写**。
    ///
    /// ⚠️ 同名头已按 RFC 7230 3.2.2 以 `", "` 合并，**这会拼坏 Set-Cookie** ——
    /// 那是该规则的著名例外（cookie 的 Expires 属性里本身就含逗号，合并后没法
    /// 可靠拆回）。本栈定位是 JSON API 客户端，不做 cookie 处理；需要完整
    /// Set-Cookie 的调用方不能用这里的 headers。
    public let headers: [String: String]

    public let body: Data

    // 以下三项让业务无需翻日志就能判断这次请求有没有真的走物理网卡
    public let peerIp: String
    /// 非 0 = 真的绑上了物理网卡。
    public let boundIfIndex: UInt32
    /// 绑法：iOS/macOS 是 "IP_BOUND_IF"，Linux 是 "SO_BINDTODEVICE"。
    public let pinMethod: String

    /// body 按 UTF-8 解出来的文本。非 UTF-8 时为 nil。
    public var bodyText: String? { String(data: body, encoding: .utf8) }
}

extension TTSignalHttpResponse {
    /// 从 C 结构体拷贝。**必须在 completion 回调内调用** —— 里面所有指针只在
    /// 回调期间有效。
    init(c: TTHttpResponse) {
        status = c.status
        reason = c.reason.map { String(cString: $0) } ?? ""

        var h = [String: String]()
        if let names = c.headerNames, let values = c.headerValues {
            for i in 0..<c.headerCount {
                guard let n = names[i], let v = values[i] else { continue }
                h[String(cString: n)] = String(cString: v)
            }
        }
        headers = h

        if let b = c.body, c.bodyLen > 0 {
            body = Data(bytes: b, count: c.bodyLen)
        } else {
            body = Data()
        }

        peerIp       = c.peerIp.map { String(cString: $0) } ?? ""
        boundIfIndex = c.boundIfIndex
        pinMethod    = c.pinMethod.map { String(cString: $0) } ?? ""
    }
}

///////////////////////////////////////////////////////////////////////////////
// continuation 蹦床
//
// C++ 侧契约：一次成功受理的请求，completion **恰好触发一次**。continuation
// 二次 resume 会直接 crash，所以这条契约是这里的安全前提。
///////////////////////////////////////////////////////////////////////////////

private final class HttpCallBox {
    let cont: CheckedContinuation<TTSignalHttpResponse, Error>
    init(_ c: CheckedContinuation<TTSignalHttpResponse, Error>) { cont = c }
}

private let ttHttpCompletion: TTHttpCompletion = { ud, error, errName, errMessage, resp in
    guard let ud else { return }
    // takeRetainedValue：与 passRetained 配平，box 在这里释放。
    let box = Unmanaged<HttpCallBox>.fromOpaque(ud).takeRetainedValue()

    guard error == 0, let resp else {
        box.cont.resume(throwing: TTSignalError(
            result:     error,
            errName:    errName.map    { String(cString: $0) },
            errMessage: errMessage.map { String(cString: $0) }))
        return
    }
    box.cont.resume(returning: TTSignalHttpResponse(c: resp.pointee))
}

private final class HttpCloseBox {
    let cont: CheckedContinuation<Void, Never>
    init(_ c: CheckedContinuation<Void, Never>) { cont = c }
}

private let ttHttpCloseDone: TTCloseDone = { ud in
    guard let ud else { return }
    Unmanaged<HttpCloseBox>.fromOpaque(ud).takeRetainedValue().cont.resume()
}

///////////////////////////////////////////////////////////////////////////////
// 连接器
///////////////////////////////////////////////////////////////////////////////

/// 一次性 HTTP/HTTPS 请求。
///
/// **复用单例，不要按请求建** —— 每个连接器都装一个日志 appender，还各自带一份
/// DNS / TLS 上下文。用完 `await close()`。
public final class TTSignalHttpConnector {

    private var handle: TTHttpConnectorRef?
    public let config: TTSignalHttpConfig

    public var isClosed: Bool { handle == nil }

    public init?(config: TTSignalHttpConfig) {
        self.config = config
        let bag = CStringBag()
        var c = TTHttpConfig()
        c.vpnPolicy          = config.net.vpnPolicy?.rawValue ?? -1
        c.caCerts            = bag.dup(config.net.caCerts)
        c.spkiPin            = bag.dup(config.net.spkiPin)
        c.insecureSkipVerify = config.net.insecureSkipVerify ? 1 : 0
        c.dnsServers         = bag.dup(config.net.dnsServers)
        c.dnsTimeoutMs       = config.net.dnsTimeoutMs
        c.drainTimeoutMs     = config.net.drainTimeoutMs
        c.logLevel           = config.net.logLevel
        c.maxResponseBytes   = config.maxResponseBytes

        // withExtendedLifetime：bag 里的 strdup 指针存进了 C struct，ARC 看不见
        // 这层依赖，最后一次用到 bag 之后就可能把它释放掉 —— 那样 C 侧读到的
        // 全是已 free 的野指针。
        guard let h = withExtendedLifetime(bag, {
            withUnsafePointer(to: &c) { tt_http_connector_create($0) }
        }) else { return nil }
        handle = h
    }

    deinit {
        if let h = handle { tt_http_connector_destroy(h) }
    }

    /// 发起一次请求。
    ///
    /// 4xx / 5xx 算**正常响应**（正常返回，不抛），与 Node 侧一致。抛出的一律是
    /// `TTSignalError`，三个字段原样透传原生层的值。
    ///
    /// - Parameters:
    ///   - timeoutMs: 覆盖 DNS + connect + TLS + 收响应的总预算。0 = 用默认。
    ///   - resolvedIp: 给了就跳过 DNS，直接连这个 IP。
    ///
    /// ⚠️ 见文件头：`await` 之后的代码会先在原生工作线程上跑一段。
    public func request(method: String = "GET",
                        url: String,
                        headers: [String: String] = [:],
                        body: Data? = nil,
                        timeoutMs: UInt32 = 0,
                        resolvedIp: String? = nil) async throws -> TTSignalHttpResponse
    {
        guard let h = handle else {
            throw TTSignalError(result: TTSignalError.Code.pinFailed,
                                errName: "BC_R_SHUTTINGDOWN",
                                errMessage: "connector already closed")
        }

        return try await withCheckedThrowingContinuation { cont in
            let bag = CStringBag()

            // 头摊成平行数组。这些指针只需活到 tt_http_request 返回 —— 原生层
            // 在调用内就把它们拷进 HttpRequest 了。
            var names:  [UnsafePointer<CChar>?] = []
            var values: [UnsafePointer<CChar>?] = []
            for (k, v) in headers {
                names.append(bag.dup(k))
                values.append(bag.dup(v))
            }

            var req = TTHttpRequest()
            req.method      = bag.dup(method)
            req.url         = bag.dup(url)
            req.headerCount = names.count
            req.timeoutMs   = timeoutMs
            req.resolvedIp  = resolvedIp.flatMap { bag.dup($0) }

            let box = Unmanaged.passRetained(HttpCallBox(cont)).toOpaque()

            let rc: Int32 = withExtendedLifetime(bag) {
              names.withUnsafeBufferPointer { np in
                values.withUnsafeBufferPointer { vp in
                    // headerCount 为 0 时 baseAddress 可能是 nil，C 侧对
                    // NULL + count=0 的组合是安全的（会跳过整个循环）。
                    req.headerNames  = np.baseAddress
                    req.headerValues = vp.baseAddress
                    let send: (UnsafePointer<TTHttpRequest>) -> Int32 = {
                        tt_http_request(h, $0, ttHttpCompletion, box)
                    }
                    if let body, !body.isEmpty {
                        return body.withUnsafeBytes { raw -> Int32 in
                            req.body    = raw.bindMemory(to: UInt8.self).baseAddress
                            req.bodyLen = raw.count
                            return withUnsafePointer(to: &req, send)
                        }
                    }
                    return withUnsafePointer(to: &req, send)
                }
              }
            }

            // 非 0 = 没受理，completion 不会来 —— 自己回收 box 并 resume，
            // 否则调用方永久挂起 + box 泄漏。契约写在 ios_bridge.h 上。
            if rc != 0 {
                Unmanaged<HttpCallBox>.fromOpaque(box).release()
                cont.resume(throwing: TTSignalError(syncResult: rc))
            }
        }
    }

    /// 关闭连接器。在途请求会被 reject，不会悬着。可重复调用。
    public func close() async {
        guard let h = handle else { return }
        handle = nil
        await withCheckedContinuation { (cont: CheckedContinuation<Void, Never>) in
            let box = Unmanaged.passRetained(HttpCloseBox(cont)).toOpaque()
            tt_http_connector_close(h, ttHttpCloseDone, box)
        }
        tt_http_connector_destroy(h)
    }
}

///////////////////////////////////////////////////////////////////////////////
// End of file : TTSignalHttp.swift
///////////////////////////////////////////////////////////////////////////////
