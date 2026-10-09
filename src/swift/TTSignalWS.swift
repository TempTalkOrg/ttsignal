///////////////////////////////////////////////////////////////////////////////
// file : TTSignalWS.swift
// author : anto
//
// WebSocket 长连接。Swift mirror of src/cpp/apple/ios_http_bridge.mm 的 tt_ws_*
// C-ABI，能力面与 Node 侧 ttsignal.createWsConnector 对齐。
//
// 形态与 TTSignalHandler 一致用 delegate（而非 async）：事件流天然是回调，
// 而且 livekit-client-swift 的 SignalingTransport 也是这个模式。
//
// ⚠️ 线程：所有回调都在该连接的事件循环线程上，**不是主线程**。实现方要碰 UI
// 得自己 hop 到 MainActor / DispatchQueue.main —— 与 TTSignalHandler 同一约定。
///////////////////////////////////////////////////////////////////////////////

import Foundation
import TTSignalC

///////////////////////////////////////////////////////////////////////////////
// 配置
///////////////////////////////////////////////////////////////////////////////

public struct TTSignalWSConfig {
    /// 与 HTTP 共享的那部分（vpnPolicy / caCerts / dns* / ...）。
    public var net = TTSignalNetConfig()

    /// 覆盖 DNS + connect + TLS + WS 握手。0 = 用原生默认（10000ms）。
    public var connectTimeoutMs: UInt32 = 0
    /// 主动 ping 间隔。0 = 不发（默认）。
    public var pingIntervalMs: UInt32 = 0
    /// 空闲多久关连接。0 = 不判（默认）。
    public var idleTimeoutMs: UInt32 = 0
    /// 收包侧单帧/单消息上限。0 = 用原生默认（8 MiB）。
    public var maxFrameBytes: Int = 0

    // 双向 TLS
    public var clientCertFile: String = ""
    public var clientKeyFile: String = ""
    public var clientKeyPassword: String = ""

    public init() {}
}

///////////////////////////////////////////////////////////////////////////////
// 事件
///////////////////////////////////////////////////////////////////////////////

public protocol TTSignalWSHandler: AnyObject {
    /// 握手结果。`error == 0` 表示 TCP + TLS + WS 握手全部完成。
    ///
    /// `message` 是原生层的现场描述 —— force-physical 失败时那段写得很细，
    /// 是排查的唯一依据。
    ///
    /// ⚠️ `error != 0` 时**不会再有 onClosed**（握手没成功，也就没有"连接关闭"
    /// 这件事）。别在 onClosed 里做本该在这里做的收尾。
    func onConnectResult(_ connection: TTSignalWSConnection,
                         error: Int32,
                         message: String?)

    /// 收到文本帧。⚠️ 原生层不校验是不是合法 UTF-8（RFC 6455 5.6 要求文本帧
    /// 必须是），需要严格性的调用方自己把关。
    func onText(_ connection: TTSignalWSConnection, text: String)

    /// 收到二进制帧。
    func onData(_ connection: TTSignalWSConnection, data: Data)

    /// 连接已关闭。只有握手成功过的连接才会走到这里。
    func onClosed(_ connection: TTSignalWSConnection, reason: String?)

    /// 引擎里冒上来的异常。
    func onException(_ connection: TTSignalWSConnection, errMsg: String)
}

// 与 TTSignalHandler 的做法一致：不是每个实现都关心全部事件。
public extension TTSignalWSHandler {
    func onText(_ connection: TTSignalWSConnection, text: String) {}
    func onData(_ connection: TTSignalWSConnection, data: Data) {}
    func onException(_ connection: TTSignalWSConnection, errMsg: String) {}
}

///////////////////////////////////////////////////////////////////////////////
// vtable
//
// box 的释放**只在 on_release 里做** —— 那是 bridge 依据 WSConnector.h 契约
// 判定的唯一释放点（见 ios_http_bridge.mm 里 TTWSConnAdapter 的注释）。其余
// 回调一律 takeUnretainedValue，碰都不碰引用计数。
///////////////////////////////////////////////////////////////////////////////

private final class WSBox {
    weak var conn: TTSignalWSConnection?
    let handler: TTSignalWSHandler
    init(handler: TTSignalWSHandler) { self.handler = handler }
}

private func wsBox(_ ud: UnsafeMutableRawPointer?) -> (WSBox, TTSignalWSConnection)? {
    guard let ud else { return nil }
    let b = Unmanaged<WSBox>.fromOpaque(ud).takeUnretainedValue()
    // conn 是 weak：业务放手之后回调可能还在路上，那时没有对象可派发，丢掉即可。
    guard let c = b.conn else { return nil }
    return (b, c)
}

private var ttWSVTable = TTWSHandlerVTable(
    on_connect_result: { ud, error, message in
        guard let (b, c) = wsBox(ud) else { return }
        b.handler.onConnectResult(c, error: error,
                                  message: message.map { String(cString: $0) })
    },
    on_text: { ud, text, _ in
        guard let (b, c) = wsBox(ud), let text else { return }
        b.handler.onText(c, text: String(cString: text))
    },
    on_data: { ud, data, len in
        guard let (b, c) = wsBox(ud), let data, len > 0 else { return }
        b.handler.onData(c, data: Data(bytes: data, count: len))
    },
    on_closed: { ud, reason in
        guard let (b, c) = wsBox(ud) else { return }
        b.handler.onClosed(c, reason: reason.map { String(cString: $0) })
    },
    on_exception: { ud, msg in
        guard let (b, c) = wsBox(ud), let msg else { return }
        b.handler.onException(c, errMsg: String(cString: msg))
    },
    on_release: { ud in
        guard let ud else { return }
        Unmanaged<WSBox>.fromOpaque(ud).release()
    }
)

private final class WSCloseBox {
    let cont: CheckedContinuation<Void, Never>
    init(_ c: CheckedContinuation<Void, Never>) { cont = c }
}

private let ttWSCloseDone: TTCloseDone = { ud in
    guard let ud else { return }
    Unmanaged<WSCloseBox>.fromOpaque(ud).takeRetainedValue().cont.resume()
}

///////////////////////////////////////////////////////////////////////////////
// 连接器
///////////////////////////////////////////////////////////////////////////////

/// WebSocket 连接器。**复用单例**，一个连接器可以开多条连接。
public final class TTSignalWSConnector {

    private var handle: TTWSConnectorRef?
    public let config: TTSignalWSConfig

    public var isClosed: Bool { handle == nil }

    public init?(config: TTSignalWSConfig) {
        self.config = config
        let bag = CStringBag()
        var c = TTWSConfig()
        c.vpnPolicy          = config.net.vpnPolicy?.rawValue ?? -1
        c.caCerts            = bag.dup(config.net.caCerts)
        c.spkiPin            = bag.dup(config.net.spkiPin)
        c.insecureSkipVerify = config.net.insecureSkipVerify ? 1 : 0
        c.dnsServers         = bag.dup(config.net.dnsServers)
        c.dnsTimeoutMs       = config.net.dnsTimeoutMs
        c.drainTimeoutMs     = config.net.drainTimeoutMs
        c.logLevel           = config.net.logLevel
        c.connectTimeoutMs   = config.connectTimeoutMs
        c.pingIntervalMs     = config.pingIntervalMs
        c.idleTimeoutMs      = config.idleTimeoutMs
        c.maxFrameBytes      = config.maxFrameBytes
        c.clientCertFile     = bag.dup(config.clientCertFile)
        c.clientKeyFile      = bag.dup(config.clientKeyFile)
        c.clientKeyPassword  = bag.dup(config.clientKeyPassword)

        // 见 TTSignalHttp.swift 同一处注释：bag 必须活到 C 调用返回。
        guard let h = withExtendedLifetime(bag, {
            withUnsafePointer(to: &c) { tt_ws_connector_create($0) }
        }) else { return nil }
        handle = h
    }

    deinit {
        if let h = handle { tt_ws_connector_destroy(h) }
    }

    /// 开一条连接。handler 由返回的 connection 持有 —— 想继续收回调就别让它被
    /// 释放掉。
    public func createConnection(handler: TTSignalWSHandler) -> TTSignalWSConnection? {
        guard let h = handle else { return nil }
        return TTSignalWSConnection(connector: h, handler: handler)
    }

    /// 关闭连接器。可重复调用。
    public func close() async {
        guard let h = handle else { return }
        handle = nil
        await withCheckedContinuation { (cont: CheckedContinuation<Void, Never>) in
            let box = Unmanaged.passRetained(WSCloseBox(cont)).toOpaque()
            tt_ws_connector_close(h, ttWSCloseDone, box)
        }
        tt_ws_connector_destroy(h)
    }
}

///////////////////////////////////////////////////////////////////////////////
// 连接
///////////////////////////////////////////////////////////////////////////////

public final class TTSignalWSConnection {

    private var handle: TTWSConnectionRef?

    public var isClosed: Bool { handle == nil }

    /// 非 0 = 真的绑上了物理网卡。握手成功后才有意义。
    public var boundIfIndex: UInt32 {
        guard let h = handle else { return 0 }
        return tt_ws_connection_bound_if(h)
    }

    fileprivate init?(connector: TTWSConnectorRef, handler: TTSignalWSHandler) {
        let box = WSBox(handler: handler)
        let ud  = Unmanaged.passRetained(box).toOpaque()
        guard let h = withUnsafePointer(to: &ttWSVTable, {
            tt_ws_connector_create_connection(connector, $0, ud)
        }) else {
            // 创建失败时 bridge 已经发过 on_release（box 在那里释放了），
            // 这里不能再 release —— 会 over-release。
            return nil
        }
        handle  = h
        box.conn = self
    }

    deinit {
        if let h = handle { tt_ws_connection_destroy(h) }
    }

    /// 发起连接。url 形如 `wss://host[:port]/path`，host 可以是域名。
    /// 结果走 `TTSignalWSHandler.onConnectResult`，**本方法只报参数/受理错误**。
    /// - Parameter timeoutMs: 0 = 用配置里的 connectTimeoutMs。
    public func connect(url: String, timeoutMs: UInt32 = 0) throws {
        guard let h = handle else {
            throw TTSignalError(result: 0, errName: "BC_R_SHUTTINGDOWN",
                                errMessage: "connection already destroyed")
        }
        let rc = tt_ws_connection_connect(h, url, timeoutMs)
        if rc != 0 { throw TTSignalError(syncResult: rc) }
    }

    /// 追加到握手请求里的头。**必须在 connect 之前调用**，之后调用会拿到
    /// `BC_R_ALREADYRUNNING`。Host / Upgrade / Connection / Sec-WebSocket-*
    /// 由原生层权威决定，给了同名项会被忽略。
    public func setRequestHeader(_ name: String, _ value: String) throws {
        guard let h = handle else { return }
        let rc = tt_ws_connection_set_request_header(h, name, value)
        if rc != 0 { throw TTSignalError(syncResult: rc) }
    }

    /// ⚠️ 抛出的错误只有 `result`，没有 errName / errMessage —— 原生的发送接口
    /// 只给错误码。（sendData / sendPing 同理。）
    public func sendText(_ text: String) throws {
        guard let h = handle else { return }
        let rc = text.withCString { tt_ws_connection_send_text(h, $0, strlen($0)) }
        if rc != 0 { throw TTSignalError(syncResult: rc) }
    }

    public func sendData(_ data: Data) throws {
        guard let h = handle, !data.isEmpty else { return }
        let rc = data.withUnsafeBytes { raw -> Int32 in
            tt_ws_connection_send_data(
                h, raw.bindMemory(to: UInt8.self).baseAddress, raw.count)
        }
        if rc != 0 { throw TTSignalError(syncResult: rc) }
    }

    public func sendPing() throws {
        guard let h = handle else { return }
        let rc = tt_ws_connection_send_ping(h)
        if rc != 0 { throw TTSignalError(syncResult: rc) }
    }

    /// 主动关闭。握手成功过的连接随后会收到 `onClosed`；没握手成功过的不会
    /// （见 `onConnectResult` 的注释）。
    public func close() {
        guard let h = handle else { return }
        tt_ws_connection_close(h)
    }
}

///////////////////////////////////////////////////////////////////////////////
// End of file : TTSignalWS.swift
///////////////////////////////////////////////////////////////////////////////
