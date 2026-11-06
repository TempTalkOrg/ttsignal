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
import org.example.android.smp.*
import java.nio.ByteBuffer
import java.nio.charset.Charset
import java.util.Timer
import java.util.TimerTask

class MainActivity : AppCompatActivity() {

    private lateinit var binding: ActivityMainBinding

    private lateinit var logTextView: TextView
    private lateinit var logScrollView: ScrollView
    private var connector: Connector? = null
    private var connection: Connection? = null
    @Volatile
    private var activeStream: Stream? = null
    private var messageTimer: Timer? = null
    private var messageCounter = 0

    
    
    
    
    
    private val initialBurstSizes = intArrayOf(173, 457, 485, 171, 44, 1729, 27, 1040)
    
    
    
    private val preRestartBurstSizes = intArrayOf(175, 196, 173, 193)
    
    
    private val postRestartBurstSizes = intArrayOf(2626, 23)

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

            if (network == pendingRestartNetwork) {
                pendingRestartNetwork = null
                connection?.let {
                    val networkHandle = network.networkHandle
                    
                    
                    sendBurst("pre-restart", preRestartBurstSizes)
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


    
    private val urls = arrayOf(
        "https://192.168.1.17:7880/rtc?protocol=13&auto_subscribe=1&adaptive_stream=1&sdk=android&version=2.20.3.6&device_model=Google Pixel 9 Pro XL&os=android&os_version=16&network=wifi",
    )

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        
        logTextView = findViewById(R.id.log_text_area)
        logScrollView = findViewById(R.id.log_scroll_view)

        
        binding.logTextArea.movementMethod = ScrollingMovementMethod.getInstance()

        
        val adapter = ArrayAdapter(this, android.R.layout.simple_spinner_item, urls)
        adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item)
        binding.urlSpinner.adapter = adapter

        
        binding.connectButton.setOnClickListener {
            if (connector == null) {
                connector = initConnector()
            }
            val selectedUrl = binding.urlSpinner.selectedItem.toString()
            createConnection(connector!!, selectedUrl)
        }

        
        binding.closeButton.setOnClickListener {
            connection?.close()
            runOnUiThread {
                binding.closeButton.isEnabled = false
            }
            connection = null
            activeStream = null
            needRestart = false
            stopBackgroundSenders()
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

            
            logScrollView.post {
                logScrollView.fullScroll(ScrollView.FOCUS_DOWN)
            }
        }
    }

    companion object {
        private const val TAG = "ttsignal-main"

        
        init {
            System.loadLibrary("signal")
        }
    }

    private fun createConnection(connector: Connector, url: String) {
        val config = Config()
        config.idleTimeOut = 10000
        config.pingInterval = 5000
        config.hostname = "localhost"
        config.port = 8003
        config.maxConnections = 1
        config.congestCtrl = Const.CC_BBR2
        config.pingOn = true
        config.alpn = "ttsignal"
        config.deviceType = 1
        config.cidTag = "12345678900"
        config.alpn = "ttsignal"
        config.serverHost = "xxx.example.org"

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
                } else {
                    appendLog("连接失败")
                }
            }

            override fun onStreamCreated(conn: Connection?, stream: Stream) {
                appendLog("新建流: " + stream.id())
                activeStream = stream
                stream.sendText("text send from client")
                
                
                
                
                
                sendBurst("connect", initialBurstSizes)
                startPeriodicPings()
            }

            override fun onStreamClosed(conn: Connection?, stream: Stream) {
                appendLog("流关闭: " + stream.id())
                if (activeStream?.id() == stream.id()) {
                    activeStream = null
                }
            }

            override fun onStreamDataAcked(conn: Connection?, stream: Stream, ackDelayTime: Long, ackedBytes: Int, inflightBytes: Int) {
                
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
                activeStream = null
                stopBackgroundSenders()
            }

            override fun onException(conn: Connection?, errorMsg: String?) {
                appendLog("连接异常: " + errorMsg)
                runOnUiThread {
                    binding.closeButton.isEnabled = false
                }
                activeStream = null
                stopBackgroundSenders()
            }

            override fun onRestart(conn: Connection?, result: Int, address: String?) {
                appendLog("重连结果: " + result + ", " + address)
                if (result == 0) {
                    
                    
                    
                    
                    sendBurst("post-restart", postRestartBurstSizes)
                }
            }
        })
        val stats = Stats()
        stats.startTime = System.currentTimeMillis()
        connection?.setUserObject(stats)
        try {
            connection?.connect(
                url,
                "{}",
                5000
            )
        } catch (e: Exception) {
            appendLog("连接异常: " + e.message)
        }
        runOnUiThread {
            binding.closeButton.isEnabled = true
        }
    }

    




    private fun sendBurst(reason: String, sizes: IntArray) {
        val stream = activeStream
        if (stream == null) {
            appendLog("[burst:$reason] skipped, no active stream")
            return
        }
        sizes.forEach { size ->
            val payload = ByteArray(size) { (it and 0xFF).toByte() }
            val r = stream.sendData(payload)
            appendLog("[burst:$reason] sendData size=$size result=$r")
        }
    }

    




    private fun startPeriodicPings() {
        messageTimer?.cancel()
        messageTimer = Timer().apply {
            scheduleAtFixedRate(object : TimerTask() {
                override fun run() {
                    val s = activeStream ?: return
                    val r7 = s.sendData(ByteArray(7) { 0x42 })
                    val r13 = s.sendData(ByteArray(13) { 0x42 })
                    appendLog("[ping] 7=$r7 13=$r13")
                }
            }, 5_000L, 5_000L)
        }
    }

    private fun stopBackgroundSenders() {
        messageTimer?.cancel()
        messageTimer = null
    }

    private fun initConnector(): Connector {
        val config = Config()
        config.taskThreads = 1
        config.timerThreads = 1
        config.idleTimeOut = 20000
        config.alpn = "ttsignal,ttsignal-ip"
        config.hostname = "localhost"
        config.port = 443
        config.maxConnections = 1000
        config.congestCtrl = Const.CC_BBR2
        config.pingOn = true
        config.numOfSenders = 1

        config.logLevel = Const.LOG_INFO
        config.logHandler = object : Config.LogHandler {
            override fun log(level: Int, msg: String?) {
                appendLog(msg)
            }
        }
        return Connector(config)
    }
}