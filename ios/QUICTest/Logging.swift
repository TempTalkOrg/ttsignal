//
//  Logging.swift
//  QUICTest
//
//  Lightweight log plumbing used by the test app:
//  - `LogLevel` mirrors the levels exposed by TTSignalConfig.LogLevel
//    so we can pipe ttsignal's native log lines through the same UI sink.
//  - `Loggable.log(...)` prints to the console *and* posts a
//    NotificationCenter notification (.ttsignalLog) consumed by ContentView.
//

import Foundation

enum LogLevel: String {
    case debug
    case info
    case warning
    case error

    static func from(_ tt: TTSignalConfig.LogLevel) -> LogLevel {
        switch tt {
        case .debug: return .debug
        case .info:  return .info
        case .warn:  return .warning
        case .error, .fatal: return .error
        }
    }
}

extension Notification.Name {
    static let ttsignalLog = Notification.Name("im.chative.ttsignal.log")
}

enum LogPayload {
    enum Keys {
        static let message = "message"
        static let level = "level"
        static let timestamp = "timestamp"
        static let source = "source"
    }

    static func post(message: String, level: LogLevel, source: String) {
        NotificationCenter.default.post(
            name: .ttsignalLog,
            object: nil,
            userInfo: [
                Keys.message: message,
                Keys.level: level.rawValue,
                Keys.timestamp: Date(),
                Keys.source: source,
            ]
        )
    }
}

protocol Loggable {}

extension Loggable {
    func log(
        _ message: String,
        _ level: LogLevel = .info,
        file _: String = #fileID,
        function: String = #function,
        line: UInt = #line
    ) {
        let source = "\(type(of: self))"
        print("[\(level.rawValue.uppercased())] \(source).\(function):\(line) \(message)")
        LogPayload.post(message: message, level: level, source: source)
    }
}
