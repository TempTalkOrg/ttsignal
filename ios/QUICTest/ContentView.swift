//
//  ContentView.swift
//  QUICTest (Plan C)
//
//  - URL + Bearer input
//  - Stat cards: state / restartCount / sent / recv / lastRestartAddress /
//    lastError
//  - Buttons: Connect / Disconnect / Send Test
//  - Live log feed (tt_logger_set_callback + ttsignalLog notifications)
//

import Combine
import SwiftUI

private let urlOptions: [String] = [
    "https://tlivekit9tcew3gy.test.chative.im/rpc/forward",
    "http://192.168.1.17:7880/rpc/forward",
]

private let defaultURL = urlOptions[0]

private let defaultBearer =
    "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9." +
    "eyJleHAiOjE4NDg2NjM3NzYsImlzcyI6ImRldmtleSIsIm5hbWUiOiJ6azEiLCJuYmYiOjE3NjIyNjM3NzYs" +
    "InN1YiI6InprMSIsInZpZGVvIjp7InJvb20iOiJyb29tMSIsInJvb21Kb2luIjp0cnVlfX0." +
    "C427_FHFvaS4_rWlJNMOhV6C0IpwL6f8v4tBwjqhpd4"

/// Choices for the log-level picker. Order intentionally matches the
/// numeric `LOG_LEVEL_*` ordering (debug = noisiest first), and the
/// labels are what the picker shows to the user. Keep `.warn` as the
/// initial selection so the demo doesn't drown the UI in xquic debug
/// spam right after launch.
private let logLevelOptions: [(level: TTSignalConfig.LogLevel, label: String)] = [
    (.debug, "DEBUG"),
    (.info,  "INFO"),
    (.warn,  "WARN"),
    (.error, "ERROR"),
    (.fatal, "FATAL"),
]

private let defaultLogLevel: TTSignalConfig.LogLevel = .warn

// MARK: - LogEntry

struct LogEntry: Identifiable, Hashable {
    let id = UUID()
    let timestamp: Date
    let level: String
    let message: String

    var formattedTime: String { Self.fmt.string(from: timestamp) }

    private static let fmt: DateFormatter = {
        let f = DateFormatter()
        f.dateFormat = "HH:mm:ss.SSS"
        return f
    }()
}

// MARK: - ContentView

struct ContentView: View {
    @StateObject private var stats = ConnectionStats()
    @State private var url: String = defaultURL
    @State private var bearer: String = defaultBearer
    @State private var logLevel: TTSignalConfig.LogLevel = defaultLogLevel
    @State private var handler: ApplicationHandler?
    @State private var logs: [LogEntry] = []
    @State private var nativeLogSinkInstalled = false

    private let logLimit = 200

    var body: some View {
        VStack(spacing: 12) {
            inputSection
            statsSection
            buttonSection
            logsSection
        }
        .padding()
        .onAppear { installNativeLogSinkIfNeeded() }
        .onChange(of: logLevel) { _, newLevel in onLogLevelChanged(newLevel) }
        .onReceive(
            NotificationCenter.default
                .publisher(for: .ttsignalLog)
                .receive(on: DispatchQueue.main)
        ) { handleLog($0) }
    }

    // MARK: subviews

    private var inputSection: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text("URL").font(.caption).foregroundStyle(.secondary)
            Menu {
                Picker("URL", selection: $url) {
                    ForEach(urlOptions, id: \.self) { Text($0).tag($0) }
                }
            } label: {
                HStack {
                    Text(url)
                        .font(.system(.callout, design: .monospaced))
                        .foregroundStyle(.primary)
                        .lineLimit(1)
                        .truncationMode(.middle)
                    Spacer()
                    Image(systemName: "chevron.up.chevron.down")
                        .foregroundStyle(.secondary)
                        .font(.caption)
                }
                .padding(.horizontal, 10)
                .padding(.vertical, 8)
                .background(Color.gray.opacity(0.12))
                .cornerRadius(6)
            }
            Text("Log Level").font(.caption).foregroundStyle(.secondary)
            Menu {
                Picker("Log Level", selection: $logLevel) {
                    ForEach(logLevelOptions, id: \.level) { opt in
                        Text(opt.label).tag(opt.level)
                    }
                }
            } label: {
                HStack {
                    Text(logLevelLabel(logLevel))
                        .font(.system(.callout, design: .monospaced))
                        .foregroundStyle(.primary)
                    Spacer()
                    Image(systemName: "chevron.up.chevron.down")
                        .foregroundStyle(.secondary)
                        .font(.caption)
                }
                .padding(.horizontal, 10)
                .padding(.vertical, 8)
                .background(Color.gray.opacity(0.12))
                .cornerRadius(6)
            }
            Text("Authorization (Bearer token, optional)")
                .font(.caption).foregroundStyle(.secondary)
            TextField("eyJ...", text: $bearer, axis: .vertical)
                .lineLimit(1 ... 3)
                .textFieldStyle(.roundedBorder)
                .autocorrectionDisabled()
                .textInputAutocapitalization(.never)
                .font(.system(.caption, design: .monospaced))
        }
    }

    private var statsSection: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                statBadge(label: "State", value: stats.state, tint: stateColor(stats.state))
                Spacer()
                statBadge(label: "Restarts", value: "\(stats.restartCount)", tint: .purple)
                Spacer()
                statBadge(label: "LocalAddr", value: stats.lastRestartAddress, tint: .blue)
            }
            HStack {
                statBadge(label: "Sent", value: "\(stats.sentCount)", tint: .indigo)
                Spacer()
                statBadge(label: "Recv", value: "\(stats.receivedCount)", tint: .indigo)
                Spacer()
            }
            if !stats.lastError.isEmpty {
                statBadge(label: "Err", value: stats.lastError, tint: .red)
            }
        }
        .padding(8)
        .background(Color.gray.opacity(0.1))
        .cornerRadius(8)
    }

    private var buttonSection: some View {
        HStack(spacing: 8) {
            Button("Connect") { connect() }
                .buttonStyle(.borderedProminent)
                .disabled(url.trimmingCharacters(in: .whitespaces).isEmpty)

            Button("Disconnect") { disconnect() }
                .buttonStyle(.bordered)
                .disabled(handler == nil)

            Button("Send Test") { sendTest() }
                .buttonStyle(.bordered)
                .disabled(handler == nil || stats.state != "CONNECTED")

            Spacer()

            Button("Clear logs") { logs.removeAll() }
                .buttonStyle(.bordered)
                .controlSize(.small)
        }
    }

    private var logsSection: some View {
        ScrollViewReader { proxy in
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 2) {
                    ForEach(logs) { entry in
                        HStack(alignment: .top, spacing: 6) {
                            Text(entry.formattedTime)
                                .font(.system(.caption2, design: .monospaced))
                                .foregroundStyle(.secondary)
                                .frame(width: 86, alignment: .leading)
                            Text(entry.level.uppercased())
                                .font(.system(.caption2, design: .monospaced))
                                .foregroundStyle(levelColor(entry.level))
                                .frame(width: 50, alignment: .leading)
                            Text(entry.message)
                                .font(.system(.caption2, design: .monospaced))
                                .frame(maxWidth: .infinity, alignment: .leading)
                                .textSelection(.enabled)
                        }
                        .id(entry.id)
                    }
                }
                .padding(6)
            }
            .background(Color.black.opacity(0.04))
            .cornerRadius(8)
            .onChange(of: logs.count) { _, _ in
                if let last = logs.last {
                    withAnimation(.linear(duration: 0.05)) {
                        proxy.scrollTo(last.id, anchor: .bottom)
                    }
                }
            }
        }
    }

    // MARK: helpers

    private func statBadge(label: String, value: String, tint: Color) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(label).font(.caption2).foregroundStyle(.secondary)
            Text(value)
                .font(.system(.caption, design: .monospaced))
                .foregroundStyle(tint)
                .lineLimit(1)
                .truncationMode(.middle)
        }
    }

    private func stateColor(_ state: String) -> Color {
        switch state {
        case "CONNECTED": .green
        case "CONNECTING": .orange
        case "FAILED": .red
        case "CLOSED": .secondary
        default: .secondary
        }
    }

    private func levelColor(_ level: String) -> Color {
        switch level.lowercased() {
        case "error": .red
        case "warning": .orange
        case "debug": .secondary
        default: .primary
        }
    }

    private func logLevelLabel(_ level: TTSignalConfig.LogLevel) -> String {
        logLevelOptions.first(where: { $0.level == level })?.label ?? "WARN"
    }

    // MARK: actions

    private func connect() {
        if let handler {
            handler.close()
            self.handler = nil
        }
        stats.reset()
        var args: [String: Any] = [:]
        let trimmed = bearer.trimmingCharacters(in: .whitespacesAndNewlines)
        if !trimmed.isEmpty {
            args["Authorization"] = "Bearer \(trimmed)"
        }
        handler = connectQUICServer(url: url,
                                    args: args,
                                    logLevel: logLevel,
                                    stats: stats)
    }

    private func disconnect() {
        handler?.close()
        handler = nil
    }

    /// The connector's log filter is fixed at construction time
    /// (SMPConnector::Init → AddExternalLogAppender), so a level change
    /// has to drop the existing connector. Tear down the active
    /// connection first (if any) so its callbacks don't race the new
    /// instance, then ask SharedConnector to clear the cached handle —
    /// the next Connect tap will rebuild it lazily with the new level.
    private func onLogLevelChanged(_ newLevel: TTSignalConfig.LogLevel) {
        let recreated = SharedConnector.resetIfLogLevelChanged(newLevel)
        guard recreated else { return }
        if let handler {
            handler.close()
            self.handler = nil
        }
        stats.reset()
        logs.append(LogEntry(timestamp: Date(),
                             level: LogLevel.info.rawValue,
                             message: "[UI] log level → \(logLevelLabel(newLevel)); connector will be rebuilt on next Connect"))
    }

    private func sendTest() {
        handler?.sendManualTest()
    }

    private func handleLog(_ note: Notification) {
        guard
            let info = note.userInfo,
            let message = info[LogPayload.Keys.message] as? String
        else { return }
        let level = info[LogPayload.Keys.level] as? String ?? LogLevel.info.rawValue
        let timestamp = info[LogPayload.Keys.timestamp] as? Date ?? Date()
        let entry = LogEntry(timestamp: timestamp, level: level, message: message)
        logs.append(entry)
        if logs.count > logLimit {
            logs.removeFirst(logs.count - logLimit)
        }
    }

    /// Pipe ttsignal native log lines (xquic / SMPConnector / AppleNetworkMonitor)
    /// through the same UI sink used by ApplicationHandler.log(...).
    private func installNativeLogSinkIfNeeded() {
        guard !nativeLogSinkInstalled else { return }
        TTSignalLog.setSink { level, message in
            LogPayload.post(message: message,
                            level: LogLevel.from(level),
                            source: "TTSignalC")
        }
        nativeLogSinkInstalled = true
    }
}

#Preview {
    ContentView()
}
