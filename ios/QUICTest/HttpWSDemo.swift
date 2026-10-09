///////////////////////////////////////////////////////////////////////////////
// file : HttpWSDemo.swift
//
// HTTP / WS binding 的验证入口。对应
// docs/superpowers/specs/2026-09-07-ios-http-ws-binding-design.md 的「验证方案」。
//
// 两种触发方式：
//   * ContentView 上的 "HTTP/WS Demo" 按钮；
//   * 启动时带环境变量 TTSIGNAL_HTTPWS_DEMO=1（给 simctl launch --console 用，
//     这样验证可以全自动跑，不必点 UI）。
///////////////////////////////////////////////////////////////////////////////

import Foundation
import TTSignalC

enum HttpWSDemo {

    static func log(_ s: String) {
        // print 会进 simctl launch --console 的输出
        print("[httpws] \(s)")
    }

    ///////////////////////////////////////////////////////////////////////////
    // 1) HTTP + vpnPolicy 对照 —— 这条是整套东西的立身之本：
    //    os 走系统默认路由（有 VPN 时出 VPN 出口 IP），forcePhysical 绕开 VPN
    //    拿到用户真实 IP。对照 Node 侧同一台机器上的结果。
    ///////////////////////////////////////////////////////////////////////////
    static func httpPolicyCompare() async {
        for (name, policy) in [("os", TTSignalVPNPolicy.os),
                               ("force-physical", .forcePhysical)] {
            var cfg = TTSignalHttpConfig()
            cfg.net.vpnPolicy = policy
            cfg.net.logLevel  = 1        // DEBUG，看原生现场
            guard let http = TTSignalHttpConnector(config: cfg) else {
                log("\(name): connector 创建失败"); continue
            }
            do {
                let r = try await http.request(url: "https://ipinfo.io/json",
                                               timeoutMs: 15000)
                let body = (r.bodyText ?? "").replacingOccurrences(
                    of: "\n", with: " ").replacingOccurrences(of: "  ", with: "")
                log("\(name): status=\(r.status) boundIf=\(r.boundIfIndex) " +
                    "pin=\(r.pinMethod.isEmpty ? "-" : r.pinMethod) peer=\(r.peerIp)")
                log("\(name): body=\(body)")
            } catch let e as TTSignalError {
                log("\(name): FAILED result=\(e.result) name=\(e.errName ?? "-")")
                log("\(name): msg=\(e.errMessage ?? "-")")
            } catch {
                log("\(name): FAILED \(error)")
            }
            await http.close()
        }
    }

    ///////////////////////////////////////////////////////////////////////////
    // 2) HTTP 错误路径 —— 确认 TTSignalError 三个字段都拿得到，尤其 errMessage
    //    （force-physical 的排查只能看那一段）。
    ///////////////////////////////////////////////////////////////////////////
    static func httpErrorPath() async {
        var cfg = TTSignalHttpConfig()
        cfg.net.vpnPolicy = .os   // 模拟器上 force-physical 恒为 64，测不到真正的错误现场
        guard let http = TTSignalHttpConnector(config: cfg) else { return }
        do {
            // 连本机：force-physical 下必然失败（路由复核不通过），
            // 正好用来看现场描述写得全不全。
            let r = try await http.request(url: "https://127.0.0.1:9/nope",
                                           timeoutMs: 5000)
            log("errorPath: 预期失败却成功了 status=\(r.status)")
        } catch let e as TTSignalError {
            log("errorPath: result=\(e.result) name=\(e.errName ?? "-")")
            log("errorPath: msg=\(e.errMessage ?? "-")")
        } catch {
            log("errorPath: \(error)")
        }
        await http.close()
    }

    ///////////////////////////////////////////////////////////////////////////
    // 3) WS 收发 + 4) on_release 计数
    ///////////////////////////////////////////////////////////////////////////
    // probe 被 binding 内部的 WSBox 强持有，而 WSBox 只在 on_release 里释放。
    // 所以「probe 被析构」等价于「on_release 触发过」—— 不必改 binding 就能
    // 验证 spec 验证方案第 4 项（每条连接恰好释放一次，不泄漏）。
    static let releaseCount = ReleaseCounter()
    final class ReleaseCounter {
        private var n = [String: Int]()
        private let lk = NSLock()
        func bump(_ tag: String) { lk.lock(); n[tag, default: 0] += 1; lk.unlock() }
        func dump() -> String {
            lk.lock(); defer { lk.unlock() }
            return n.sorted { $0.key < $1.key }
                    .map { "\($0.key)=\($0.value)" }.joined(separator: " ")
        }
    }

    final class WSProbe: TTSignalWSHandler {
        deinit { HttpWSDemo.releaseCount.bump(tag) }
        let tag: String
        let done: (String) -> Void
        private var finished = false
        // ⚠️ 必须由调用方持有 connection：binding 里 WSBox.conn 是 weak，
        // 没人强引用的话回调找不到对象，会被静默丢弃。
        var conn: TTSignalWSConnection?

        init(tag: String, done: @escaping (String) -> Void) {
            self.tag = tag; self.done = done
        }

        func onConnectResult(_ c: TTSignalWSConnection, error: Int32, message: String?) {
            HttpWSDemo.log("\(tag): connectResult error=\(error) boundIf=\(c.boundIfIndex)")
            if error != 0 {
                HttpWSDemo.log("\(tag): msg=\(message ?? "-")")
                finish("connect-failed")
                return
            }
            do { try c.sendText("hello-from-ios") }
            catch { HttpWSDemo.log("\(tag): sendText 失败 \(error)") }
        }

        func onText(_ c: TTSignalWSConnection, text: String) {
            HttpWSDemo.log("\(tag): recv text=\(text)")
            c.close()
        }

        func onClosed(_ c: TTSignalWSConnection, reason: String?) {
            HttpWSDemo.log("\(tag): closed reason=\(reason ?? "-")")
            finish("closed")
        }

        func onException(_ c: TTSignalWSConnection, errMsg: String) {
            HttpWSDemo.log("\(tag): exception \(errMsg)")
        }

        private func finish(_ how: String) {
            guard !finished else { return }
            finished = true
            done(how)
        }
    }

    static func wsEcho(url: String, tag: String, policy: TTSignalVPNPolicy) async {
        var cfg = TTSignalWSConfig()
        cfg.net.vpnPolicy = policy
        cfg.connectTimeoutMs = 10000
        guard let ws = TTSignalWSConnector(config: cfg) else {
            log("\(tag): connector 创建失败"); return
        }
        await withCheckedContinuation { (cont: CheckedContinuation<Void, Never>) in
            var resumed = false
            let probe = WSProbe(tag: tag) { _ in
                guard !resumed else { return }
                resumed = true
                cont.resume()
            }
            guard let conn = ws.createConnection(handler: probe) else {
                log("\(tag): createConnection 失败")
                resumed = true; cont.resume(); return
            }
            probe.conn = conn        // 持有到回调结束
            do { try conn.connect(url: url, timeoutMs: 10000) }
            catch {
                log("\(tag): connect 受理失败 \(error)")
                if !resumed { resumed = true; cont.resume() }
            }
        }
        await ws.close()
    }

    ///////////////////////////////////////////////////////////////////////////

    ///////////////////////////////////////////////////////////////////////////
    // 回归用例：业务发起 connect 之后**立刻丢掉** connection 引用。
    //
    // 这是 crash-2026.09.07.01 的复现路径：连接还在握手，TTSignalWSConnection
    // 已经 deinit -> tt_ws_connection_destroy，若 bridge 在那里直接 delete
    // adapter，随后到达的 OnChannelClosed -> _DeliverConnectResult 就会回调到
    // 已释放的 handler（EXC_BAD_ACCESS）。
    //
    // 正确行为：不崩溃。回调找不到 Swift 对象时被安全丢弃。
    ///////////////////////////////////////////////////////////////////////////
    static func wsAbandonImmediately() async {
        var cfg = TTSignalWSConfig()
        cfg.net.vpnPolicy = .os
        cfg.connectTimeoutMs = 5000
        guard let ws = TTSignalWSConnector(config: cfg) else { return }
        do {
            let probe = WSProbe(tag: "ws-abandon") { _ in }
            guard let conn = ws.createConnection(handler: probe) else { return }
            try conn.connect(url: "wss://echo.websocket.org/", timeoutMs: 5000)
            // 故意不持有 conn / probe：出了这个作用域两者都会被释放
        } catch {
            log("ws-abandon: connect 受理失败 \(error)")
        }
        // 给回调一点时间到达（若有 UAF，这段时间内就会崩）
        try? await Task.sleep(nanoseconds: 3_000_000_000)
        log("ws-abandon: 存活（没有 UAF）")
        await ws.close()
    }

    ///////////////////////////////////////////////////////////////////////////
    // 压测用例：放弃连接这条路径跑量，顺带核对释放记账。
    //
    // 连 127.0.0.1:9（TCP 立刻被拒），每轮建连接、发起 connect、马上丢引用，
    // 让 destroy 与事件线程上的收尾回调尽量挨在一起。判据有两条：
    //   * 不崩、ASan 不报（整条放弃路径在高频下依然干净）；
    //   * ws-storm 计数恰好等于轮数（每条连接的 on_release 不多不少一次）。
    //
    // ⚠️ 说清楚它测不到什么：adapter 销毁归属的那个竞态
    // （ReleaseOnce 放锁后、读 vt/userdata 前，DetachFromOwner 抢先 delete）
    // 窗口只有几条指令宽，且老代码在那之后只读局部变量，从 Swift 这一侧根本
    // 观察不到。那个竞态的确定性验证在
    //     src/cpp/tests/IosWSAdapterRelease_test.mm
    // 里（用例 4，ASan 下必报 heap-use-after-free）。这里只负责证明真实调用
    // 序列在量上是干净的。
    ///////////////////////////////////////////////////////////////////////////
    static func wsAbandonStorm(rounds: Int = 300) async {
        var cfg = TTSignalWSConfig()
        cfg.net.vpnPolicy    = .os
        cfg.connectTimeoutMs = 1000
        guard let ws = TTSignalWSConnector(config: cfg) else {
            log("ws-storm: connector 创建失败"); return
        }
        for _ in 0..<rounds {
            let probe = WSProbe(tag: "ws-storm") { _ in }
            guard let conn = ws.createConnection(handler: probe) else { break }
            try? conn.connect(url: "ws://127.0.0.1:9/", timeoutMs: 1000)
            // conn / probe 出作用域立刻释放
        }
        // 等在途回调走完再统计
        try? await Task.sleep(nanoseconds: 3_000_000_000)
        log("ws-storm: \(rounds) 轮存活（放弃路径无崩溃）")
        await ws.close()
    }

    static func runAll() async {
        // 环境自检：模拟器共享宿主 Mac 的网络栈，active interface 往往是
        // 宿主的 VPN 隧道，force-physical 因此会（正确地）失败。真机上才应该
        // 拿得到物理网卡。
        log("netmon ifindex: os=\(tt_netmon_query_default_ifindex_ex(0)) " +
            "prefer=\(tt_netmon_query_default_ifindex_ex(1)) " +
            "force=\(tt_netmon_query_default_ifindex_ex(2))")
        log("===== HTTP: vpnPolicy 对照 =====")
        await httpPolicyCompare()
        log("===== HTTP: 错误路径 =====")
        await httpErrorPath()
        // 用 os 策略：模拟器上 force-physical 必然拿不到物理网卡（见上面的
        // 自检），那样测不到收发本身。force-physical 的行为由上面的 HTTP 段覆盖。
        log("===== WS: echo（os）=====")
        await wsEcho(url: "wss://echo.websocket.org/",
                     tag: "ws-echo", policy: .os)
        // WS 的**同步**失败路径：模拟器上 force-physical 必然在 TcpChannel::Open
        // 里硬失败（64），connect() 当场抛。看的是抛出来的 error 带不带现场描述。
        log("===== WS: 同步失败带现场描述（force-physical）=====")
        await wsEcho(url: "wss://echo.websocket.org/",
                     tag: "ws-sync-fail", policy: .forcePhysical)
        log("===== WS: 连接失败路径（确认不泄漏、且无 onClosed）=====")
        await wsEcho(url: "wss://127.0.0.1:9/nope",
                     tag: "ws-fail", policy: .os)
        log("===== WS: 提前放弃连接（crash 回归）=====")
        await wsAbandonImmediately()
        log("===== WS: 放弃路径压测 + 释放记账 =====")
        await wsAbandonStorm()
        // 等一轮，让最后那条连接的 on_release 走完
        try? await Task.sleep(nanoseconds: 1_000_000_000)
        log("on_release 计数（每条连接应恰好 1）: \(releaseCount.dump())")
        log("===== 全部结束 =====")
    }
}
