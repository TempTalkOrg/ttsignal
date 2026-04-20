# 多 ALPN 使用示例

ttsignal 支持在 Connector 级别注册多个 ALPN，然后在 Connection 级别按需选择：

- **`ttsignal`** — 用于权威域名访问（服务端有合法 TLS 证书）
- **`ttsignal-ip`** — 用于自签名证书 IP 直连场景

## Node.js

```js
const tts = require('ttsignal');

// 1. createConnector：注册所有需要的 ALPN（逗号分隔）
const connector = tts.createConnector({
    alpn: 'ttsignal,ttsignal-ip',
    ping_on: true,
    idle_time_out: 20000,
    log_level: tts.LOG_LEVEL_WARN,
    taskThreads: 1
});

// 2a. 权威域名 → 使用 ttsignal
const connDomain = connector.createConnection({
    alpn: 'ttsignal'
});
connDomain.connect('https://livekit.example.com/rtc', {
    Authorization: 'Bearer <token>'
}, 10000, (err, resp) => {
    if (err) return console.error('connect error', err);
    console.log('domain connected', resp);
});

// 2b. 自签名 IP 直连 → 使用 ttsignal-ip
const connIP = connector.createConnection({
    alpn: 'ttsignal-ip'
});
connIP.connect('https://192.168.1.17:7880/rtc', {
    Authorization: 'Bearer <token>'
}, 10000, (err, resp) => {
    if (err) return console.error('connect error', err);
    console.log('ip connected', resp);
});
```

## Kotlin (Android)

```kotlin
// 1. createConnector：注册所有需要的 ALPN（逗号分隔）
val connectorConfig = Config().apply {
    taskThreads = 1
    timerThreads = 1
    idleTimeOut = 20000
    alpn = "ttsignal,ttsignal-ip"
    hostname = "localhost"
    port = 443
    maxConnections = 1000
    congestCtrl = Const.CC_BBR2
    pingOn = true
    logLevel = Const.LOG_WARN
}
val connector = Connector(connectorConfig)

// 2a. 权威域名 → 使用 ttsignal
val domainConfig = Config().apply {
    idleTimeOut = 20000
    maxConnections = 1000
    congestCtrl = Const.CC_BBR2
    pingOn = true
    alpn = "ttsignal"
}
val connDomain = connector.createConnection(domainConfig, object : IConnectionHandler {
    override fun onConnectResult(conn: Connection?, error: Int, message: String?) {
        Log.d("TTS", "domain connect result: $error $message")
    }
    // ... 其他回调
})
connDomain.connect(
    "https://livekit.example.com/rtc",
    """{"Authorization":"Bearer <token>"}""",
    10000
)

// 2b. 自签名 IP 直连 → 使用 ttsignal-ip
val ipConfig = Config().apply {
    idleTimeOut = 20000
    maxConnections = 1000
    congestCtrl = Const.CC_BBR2
    pingOn = true
    alpn = "ttsignal-ip"
}
val connIP = connector.createConnection(ipConfig, object : IConnectionHandler {
    override fun onConnectResult(conn: Connection?, error: Int, message: String?) {
        Log.d("TTS", "ip connect result: $error $message")
    }
    // ... 其他回调
})
connIP.connect(
    "https://192.168.1.17:443/rtc",
    """{"Authorization":"Bearer <token>"}""",
    10000
)
```

## 要点

| 层级 | alpn 值 | 说明 |
|------|---------|------|
| `createConnector` | `"ttsignal,ttsignal-ip"` | 注册引擎支持的所有 ALPN，逗号分隔 |
| `createConnection` | `"ttsignal"` | 该连接使用权威域名，服务端有合法证书 |
| `createConnection` | `"ttsignal-ip"` | 该连接使用 IP 直连，服务端自签名证书 |

> **注意**：Connection 级别指定的 `alpn` 必须是 Connector 已注册的 ALPN 之一，否则连接会失败。
