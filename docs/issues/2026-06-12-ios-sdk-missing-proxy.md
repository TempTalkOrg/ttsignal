# iOS SDK 缺少 MASQUE proxy 字段

## 原始问题

用户反馈：app 集成 iOS SDK 时报错说没有 `proxy` 字段。需要确认 MASQUE
proxy 字段到底有没有集成进 iOS SDK。

## 表现 / 现象

- 主仓 `ttsignal` 的源码层已经完整包含 MASQUE proxy 配置字段：
  - `src/swift/TTSignalConfig.swift`：`proxyUrl` / `proxyHost` / `proxyPort`
    / `proxySni` / `proxyCaCertPem` / `spkiPin`，并在 `withCConfig` 中逐项
    填进 `TTConfig`（见第 74-87、152-157 行）。
  - `src/cpp/apple/ios_bridge.h`：`TTConfig` 结构体含同名 proxy 字段
    （第 68-77 行）。
  - `src/cpp/apple/ios_bridge.mm::ConvertConfig`：把这些字段写进
    `BCFObject`（`proxy_url` / `proxy_host` / `proxy_port` / `proxy_sni`
    / `proxy_ca_cert_pem` / `spki_pin`，第 87-108 行）。
- 本地已构建的 xcframework（`build/ios-xcframework/.../Headers/ios_bridge.h`，
  6/12 10:23 重建）头文件含 14 处 `proxy`，即本地二进制 C 层已带 proxy。
- 但 app 实际集成的来源是独立发布仓
  `3th1UOYgUtJkurSZ/ttsignal-xcframework`（SwiftPM / CocoaPods），其
  Swift 源码层 **不含** proxy 字段：
  - 远程最新 release tag `1.0.20260612`（commit `099c4079`）的
    `Sources/TTSignal/TTSignalConfig.swift` 中 proxy 计数 = **0**。
  - 远程默认分支 HEAD 的 `TTSignalConfig.swift` 中 proxy 计数 = **0**。
- 本地 release-repo checkout（`build/release-repo-checkout`）的 tag
  `1.0.20260612` 指向 commit `ffcb7b8`（proxy 计数 = 20），与远程同名
  tag 指向的 `099c4079` 不一致 —— 本地领先远程，发布未推送。

## 初步调查 / 根因初判

iOS SDK 的分发链路是双轨的：

1. **二进制 xcframework（C 桥接 + `.a`）** 由
   `ios/scripts/release-xcframework.sh --upload` 打 zip 上传到 release 仓的
   GitHub Release 资产，SwiftPM 通过 `binaryTarget(url:checksum:)` 拉取。
2. **Swift 绑定源码（`src/swift/*.swift`，含 `TTSignalConfig` 的 proxy
   字段）** 由 `ios/scripts/publish-to-release-repo.sh` 用 `rsync` 同步到
   release 仓的 `Sources/TTSignal/`，再 commit + tag + `--push`。

proxy 字段是 6/11 才加入主仓的：
- `741ad37` feat(masque): expose proxy config in iOS/Android/Swift bindings
- `5178ebd` feat(masque): align iOS/Swift proxy config with proxyCaCertPem / spkiPin

**根因初判（已验证的事实）**：含 proxy 的 Swift 源至今没有真正发布到
release 仓。`publish-to-release-repo.sh` 只在本地跑出了 commit `ffcb7b8`
和 tag（dry-run，没 `--push`），远程 release 仓的 main 与所有 tag（含最新
`1.0.20260612`）的 `TTSignalConfig.swift` 仍是不含 proxy 的旧版本。app 通过
SwiftPM 从 release 仓拉到的 Swift API 层因此没有 `proxy*` 属性，编译期即
报"没有 proxy 字段"。

> 待确认（尚为假设）：远程 release `1.0.20260612` 的 zip 资产（C 层二进制）
> 是 6/12 02:23Z 上传的，时间晚于 6/11 的 proxy commit，故二进制 C 层
> **可能**已含 proxy；但这不影响结论 —— app 报错的是 Swift API 符号缺失，
> 取决于 `Sources/TTSignal/` 的源码，而它确实缺 proxy。

## 修复计划

把含 proxy 的版本完整发布到 release 仓 `3th1UOYgUtJkurSZ/ttsignal-xcframework`，
让 app 能通过 SwiftPM/CocoaPods 拉到带 proxy 的 Swift API：

1. 确认本地 xcframework 已基于含 proxy 的源重建（已满足：`build/ios-xcframework`
   6/12 10:23，C 头含 proxy）。如不放心可重跑：
   `ios/scripts/build-core.sh` + `ios/scripts/build-xcframework.sh`。
2. 重新打包并上传二进制资产（会按当天日期自动定版，可能 bump 为
   `1.0.20260612-1`）：
   `ios/scripts/release-xcframework.sh --upload`
3. 把含 proxy 的 Swift 源同步并推送到 release 仓：
   `ios/scripts/publish-to-release-repo.sh --push`
   （首次会因为本地 tag 领先而触发 tag 移动；脚本在资产 SHA 一致时会自动
   允许）。
4. 验证：
   - `gh api repos/3th1UOYgUtJkurSZ/ttsignal-xcframework/contents/Sources/TTSignal/TTSignalConfig.swift?ref=<新tag>`
     解码后应能 grep 到 proxy 字段。
   - app 端把 SwiftPM 依赖更新到新 tag（或 `from:` 拉到新版本）后重新编译，
     `TTSignalConfig().proxyUrl = ...` 等应可用。
5. 回滚思路：发布是追加 tag + release 资产，不破坏旧版本；如新版本有问题，
   app 端把依赖 pin 回旧 tag 即可。

> 注意（版本号语义）：若自动 bump 出 `-N` 后缀（如 `1.0.20260612-1`），它在
> SemVer 里属 pre-release，SwiftPM `from:` 不会自动选中，app 需显式 pin 该
> tag。详见 `release-xcframework.sh` 头部注释。
