package com.example.ttsignal

import android.content.Context
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.LinkProperties
import android.net.NetworkRequest
import android.os.Build
import android.os.Bundle
import android.util.Log
import android.text.method.ScrollingMovementMethod
import android.widget.ArrayAdapter
import android.widget.ScrollView
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import com.example.ttsignal.databinding.ActivityMainBinding
import org.difft.android.smp.*
import java.nio.ByteBuffer
import java.nio.charset.Charset
import java.util.Timer

class MainActivity : AppCompatActivity() {

    private lateinit var binding: ActivityMainBinding

    private lateinit var logTextView: TextView
    private lateinit var logScrollView: ScrollView
    private var connector: Connector? = null
    private var connection: Connection? = null
    private var messageTimer: Timer? = null
    private var messageCounter = 0

    private var connectivityManager: ConnectivityManager? = null

    @Volatile
    private var activeNetwork: Network? = null
    @Volatile
    private var needRestart = false
    @Volatile
    private var pendingRestartNetwork: Network? = null

    private val networkCallback = object : ConnectivityManager.NetworkCallback() {
        override fun onAvailable(network: Network) {
            appendLog("[网络] 可用 network=$network")

            val prev = activeNetwork
            activeNetwork = network
            if (prev != activeNetwork || needRestart) {
                needRestart = false
                pendingRestartNetwork = network
                appendLog("[网络] 网络切换，等待 validated 后 restart network=$network")
            }
        }

        override fun onLost(network: Network) {
            appendLog("[网络] 丢失 network=$network")

            if (network == activeNetwork) {
                activeNetwork = null
                pendingRestartNetwork = null
                if (connection != null) {
                    needRestart = true
                    appendLog("[网络] 当前网络丢失，等待新网络后 restart")
                }
            }
        }

        override fun onCapabilitiesChanged(network: Network, caps: NetworkCapabilities) {
            val transports = buildTransportLabel(caps)
            appendLog("[网络] 能力变化 transports=$transports network=$network")
            activeNetwork = network

            if (network == pendingRestartNetwork
                && caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_VALIDATED)) {
                pendingRestartNetwork = null
                connection?.let {
                    val networkHandle = network.networkHandle
                    appendLog("[网络] 网络已验证，执行 restart network=$network networkHandle=$networkHandle")
                    it.restart(networkHandle)
                }
            }
        }

        override fun onLinkPropertiesChanged(
            network: Network,
            linkProperties: LinkProperties
        ) {
            appendLog("[网络] 链路属性变化 network=$network")
        }
    }

    class Stats{
        public var startTime: Long = 0
        public var connectedTime: Long = 0
        public var endTime: Long = 0
    }


    // List of URLs for the dropdown
    private val urls = arrayOf(
        "https://tlivekit9tcew3gy.test.chative.im/rpc/forward",
        "https://192.168.1.17:7880/rtc?protocol=13&auto_subscribe=1&adaptive_stream=1&sdk=android&version=2.20.3.6&device_model=Google Pixel 9 Pro XL&os=android&os_version=16&network=wifi",
        "https://tlivekit9tcew3gy.test.chative.im/rtc?protocol=13&auto_subscribe=1&adaptive_stream=1&sdk=android&version=2.20.3.6&device_model=Google Pixel 9 Pro XL&os=android&os_version=16&network=wifi",
    )

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        // 通过ID找到你的视图
        logTextView = findViewById(R.id.log_text_area)
        logScrollView = findViewById(R.id.log_scroll_view)

        // Make the log text area scrollable
        binding.logTextArea.movementMethod = ScrollingMovementMethod.getInstance()

        // Populate the spinner
        val adapter = ArrayAdapter(this, android.R.layout.simple_spinner_item, urls)
        adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item)
        binding.urlSpinner.adapter = adapter

        // Set up the connect button
        binding.connectButton.setOnClickListener {
            if (connector == null) {
                connector = initConnector()
            }
            val selectedUrl = binding.urlSpinner.selectedItem.toString()
            createConnection(connector!!, selectedUrl)
        }

        // Set up the close button
        binding.closeButton.setOnClickListener {
            connection?.close()
            runOnUiThread {
                binding.closeButton.isEnabled = false
            }
            connection = null
            needRestart = false
            messageTimer?.cancel()
            messageTimer = null
        }
    }

    override fun onStart() {
        super.onStart()
        val cm = getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
        connectivityManager = cm
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
            cm.registerDefaultNetworkCallback(networkCallback)
        } else {
            val request = NetworkRequest.Builder()
                .addCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
                .build()
            cm.registerNetworkCallback(request, networkCallback)
        }
    }

    override fun onStop() {
        connectivityManager?.unregisterNetworkCallback(networkCallback)
        connectivityManager = null
        super.onStop()
    }

    private fun buildTransportLabel(caps: NetworkCapabilities): String {
        return buildString {
            if (caps.hasTransport(NetworkCapabilities.TRANSPORT_WIFI)) append("WiFi ")
            if (caps.hasTransport(NetworkCapabilities.TRANSPORT_CELLULAR)) append("蜂窝 ")
            if (caps.hasTransport(NetworkCapabilities.TRANSPORT_ETHERNET)) append("以太网 ")
            if (caps.hasTransport(NetworkCapabilities.TRANSPORT_VPN)) append("VPN ")
            if (isEmpty()) append("未知")
        }.trim()
    }

    private fun appendLog(message: String?) {
        runOnUiThread {
            binding.logTextArea.append(message+"\n")
            Log.i(TAG, message + "")

            // 使用post来确保滚动操作在布局更新后执行
            logScrollView.post {
                logScrollView.fullScroll(ScrollView.FOCUS_DOWN)
            }
        }
    }

    companion object {
        private const val TAG = "ttsignal-main"

        // Used to load the 'signal' library on application startup.
        init {
            System.loadLibrary("signal")
        }
    }

    private fun createConnection(connector: Connector, url: String) {
        val config = Config()
        config.idleTimeOut = 20000
        config.hostname = "localhost"
        config.port = 8003
        config.maxConnections = 1000
        config.congestCtrl = Const.CC_BBR2
        config.pingOn = true
        config.alpn = "ttsignal"
        config.deviceType = 1
        config.cidTag = "12345678900"

        val charset = Charset.forName("UTF-8")
        connection?.close()
        connection = connector.createConnection(config, object : IConnectionHandler {
            override fun onConnectResult(
                conn: Connection?,
                errorCode: Int,
                message: String?
            ) {
                appendLog(
                    "连接结果(pid:" + Thread.currentThread()
                        .getId() + "): " + errorCode + ", " + message
                )
                if (errorCode == 0) {
                    val stats = conn?.userObject as Stats
                    stats.connectedTime = System.currentTimeMillis()
                    appendLog("连接成功, 耗时：" + (stats.connectedTime - stats.startTime) + "ms")
                    runOnUiThread {
                        binding.closeButton.isEnabled = true
                    }
//                    messageTimer?.cancel()
//                    messageTimer = Timer()
//                    messageCounter = 0
//                    messageTimer?.schedule(object : TimerTask() {
//                        override fun run() {
//                            connection?.let {
//                                val stream = it.createStream()
//                                val msg = "ping from client ${++messageCounter}"
//                                stream.sendText(msg)
//                                appendLog("Sent message: $msg")
//                            }
//                        }
//                    }, 0, 2000)
                } else {
                    appendLog("连接失败")
                }
            }

            override fun onStreamCreated(conn: Connection?, stream: Stream) {
                appendLog("新建流: " + stream.id())
                stream.sendText("text send from client")
            }

            override fun onStreamClosed(conn: Connection?, stream: Stream) {
                appendLog("流关闭: " + stream.id())
            }

            override fun onStreamDataAcked(conn: Connection?, stream: Stream, ackDelayTime: Long, ackedBytes: Int, inflightBytes: Int) {
                // appendLog("流包确认: " + ackDelayTime + ", " + ackedBytes + ", " + inflightBytes)
            }

            override fun onStreamDataSent(conn: Connection?, stream: Stream, transId: Int, size: Int) {
                appendLog("流数据发送: " + transId + ", " + size)
            }

            override fun onRecvCmd(
                conn: Connection?,
                timestamp: Long,
                transId: Int,
                stream: Stream?,
                buffer: ByteArray
            ) {
                appendLog("收到命令: " + timestamp + ", " + transId)
                val cmd = charset.decode(ByteBuffer.wrap(buffer)).toString()
                appendLog("命令内容: " + cmd)
            }

            override fun onRecvData(
                conn: Connection?,
                timestamp: Long,
                transId: Int,
                stream: Stream?,
                buffer: ByteArray
            ) {
                appendLog("收到数据: " + timestamp + ", " + transId)
                val data = charset.decode(ByteBuffer.wrap(buffer)).toString()
                appendLog("数据内容: " + data)
            }

            override fun onClosed(conn: Connection?, reason: String?) {
                val stats = conn?.userObject as Stats
                val endTime = System.currentTimeMillis()
                var connectedTime = stats.connectedTime
                if (connectedTime == 0.toLong())
                    connectedTime = endTime
                stats.endTime = endTime
                appendLog("连接关闭(pid:" + Thread.currentThread().getId() + "): " + reason+",持续时长："+(endTime-connectedTime)+"ms")
                runOnUiThread {
                    binding.closeButton.isEnabled = false
                }
                messageTimer?.cancel()
                messageTimer = null
            }

            override fun onException(conn: Connection?, errorMsg: String?) {
                appendLog("连接异常: " + errorMsg)
                runOnUiThread {
                    binding.closeButton.isEnabled = false
                }
                messageTimer?.cancel()
                messageTimer = null
            }

            override fun onRestart(conn: Connection?, result: Int, address: String?) {
                appendLog("重连结果: " + result + ", " + address)
            }
        })
        val stats = Stats()
        stats.startTime = System.currentTimeMillis()
        connection?.setUserObject(stats)
        try {
            connection?.connect(
                url,
                "{\"Authorization\":\"Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJleHAiOjE4NDg2NjM3NzYsImlzcyI6ImRldmtleSIsIm5hbWUiOiJ6azEiLCJuYmYiOjE3NjIyNjM3NzYsInN1YiI6InprMSIsInZpZGVvIjp7InJvb20iOiJyb29tMSIsInJvb21Kb2luIjp0cnVlfX0.C427_FHFvaS4_rWlJNMOhV6C0IpwL6f8v4tBwjqhpd4\"}",
                5000
            )
        } catch (e: Exception) {
            appendLog("连接异常: " + e.message)
        }
        runOnUiThread {
            binding.closeButton.isEnabled = true
        }
    }

    private fun initConnector(): Connector {
        val config = Config()
        config.taskThreads = 1
        config.timerThreads = 1
        config.idleTimeOut = 20000
        config.alpn = "ttsignal"
        config.hostname = "localhost"
        config.port = 4433
        config.maxConnections = 1000
        config.congestCtrl = Const.CC_BBR2
        config.pingOn = true
//        config.logFile = "ttclient.log"
        config.logLevel = Const.LOG_WARN
        config.logHandler = object : Config.LogHandler {
            override fun log(level: Int, msg: String?) {
                appendLog(msg)
            }
        }
        return Connector(config)
    }
}