//
//  QUICTestApp.swift
//  QUICTest
//
//  方案 A 验证 app 入口。SwiftUI Scene。
//

import SwiftUI

@main
struct QUICTestApp: App {
    var body: some Scene {
        WindowGroup {
            ContentView()
                .task {
                    // 带 TTSIGNAL_HTTPWS_DEMO=1 启动时自动跑 HTTP/WS 验证，
                    // 输出走 print -> simctl launch --console。这样验证可以
                    // 全自动，不必点 UI。
                    if ProcessInfo.processInfo
                        .environment["TTSIGNAL_HTTPWS_DEMO"] == "1" {
                        await HttpWSDemo.runAll()
                    }
                }
        }
    }
}
