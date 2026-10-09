///////////////////////////////////////////////////////////////////////////////
// file : TTSignalError.swift
// author : anto
//
// HTTP / WebSocket 绑定层的错误类型。三个字段**原样透传**原生层给的值，不重新
// 包装 —— 与 Node 侧 (src/js/index.js) 同一约定。
///////////////////////////////////////////////////////////////////////////////

import Foundation
import TTSignalC

public struct TTSignalError: Error {

    /// BCRESULT 数值，跨语言契约值。常用取值见 `Code`。
    public let result: Int32

    /// 错误码的符号名，如 `"BC_R_ROUTE_MISMATCH"`。表外的码形如 `"BC_R_(123)"`。
    /// 稳定、可 grep、可分支 —— 要判断错误种类用它，别去 parse errMessage。
    public let errName: String?

    /// 原生层的现场描述。
    ///
    /// **排查 force-physical 只能看这一段** —— 它写得很细，包含对端 IP、
    /// peerClass、内核把包判给了哪块网卡、以及该怎么改配置。通用提示给不出这些
    /// 细节，所以不要吞掉、也不要改写它。
    ///
    /// ⚠️ 发送类接口（`sendText` / `sendData` / `sendPing`）抛出的错误只有
    /// `result`，`errName` / `errMessage` 都是 nil —— 原生的发送接口只给错误码，
    /// 不给文案。
    public let errMessage: String?

    public init(result: Int32,
                errName: String? = nil,
                errMessage: String? = nil) {
        self.result     = result
        self.errName    = errName
        self.errMessage = errMessage
    }

    /// 同步返回错误码的路径用这个 —— 那些路径上没有 completion 可以带
    /// errName / errMessage，两者都向 C 侧取：名字查那份共用的错误码表，
    /// 现场描述取 `tt_last_sync_error()`。都不在 Swift 侧另抄一份。
    ///
    /// ⚠️ 必须在失败的那次 C 调用**之后立刻**构造：`tt_last_sync_error()` 是
    /// 线程局部的「最近一次」，下一次同步调用就覆盖了。
    init(syncResult: Int32) {
        self.result  = syncResult
        self.errName = String(cString: tt_result_name(syncResult))
        let msg = String(cString: tt_last_sync_error())
        self.errMessage = msg.isEmpty ? nil : msg
    }

    /// 这条栈实际会产生的错误码。数值是跨语言契约值，与 src/js/index.js 的
    /// TTS_R_* 常量、src/cpp/TTErrors.h 一致，不要改动。
    public enum Code {
        /// 当下没有可用物理网卡（active 路径可能只有 VPN 隧道）。
        public static let noPhysicalInterface: Int32 = 64
        /// 把 socket 绑到物理网卡失败。
        public static let pinFailed: Int32           = 69
        /// 路由复核不通过 —— 对端从物理网卡出不去（连环回/内网即是这个）。
        public static let routeMismatch: Int32       = 70
        public static let dnsFailed: Int32           = 71
        public static let tlsVerifyFailed: Int32     = 72
        /// 响应超过 maxResponseBytes（仅 HTTP）。
        public static let responseTooLarge: Int32    = 73
        /// WebSocket 握手失败（仅 WS）。
        public static let wsHandshakeFailed: Int32   = 74
    }
}

extension TTSignalError: CustomStringConvertible {
    public var description: String {
        let name = errName ?? "BC_R_(\(result))"
        guard let msg = errMessage, !msg.isEmpty else {
            return "TTSignalError(\(result) \(name))"
        }
        return "TTSignalError(\(result) \(name)): \(msg)"
    }
}

///////////////////////////////////////////////////////////////////////////////
// End of file : TTSignalError.swift
///////////////////////////////////////////////////////////////////////////////
