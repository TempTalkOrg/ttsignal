package com.example.ttsignal

import android.os.Bundle
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

    class Stats{
        public var startTime: Long = 0
        public var connectedTime: Long = 0
        public var endTime: Long = 0
    }


    // List of URLs for the dropdown
    private val urls = arrayOf(
        "https://192.168.1.3:4433/webtransport",
        "https://192.168.1.3:7880/rtc?protocol=13&auto_subscribe=1&adaptive_stream=1&sdk=android&version=2.20.3.6&device_model=Google Pixel 9 Pro XL&os=android&os_version=16&network=wifi",
        "https://tlivekit9tcew3gy.test.chative.im/rtc?protocol=13&auto_subscribe=1&adaptive_stream=1&sdk=android&version=2.20.3.6&device_model=Google Pixel 9 Pro XL&os=android&os_version=16&network=wifi",
        "https://tlivekit9tcew3gy.test.chative.im:7883/webtransport"
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
            messageTimer?.cancel()
            messageTimer = null
        }
    }

    private fun appendLog(message: String?) {
        runOnUiThread {
            binding.logTextArea.append(message+"\n")

            // 使用post来确保滚动操作在布局更新后执行
            logScrollView.post {
                logScrollView.fullScroll(ScrollView.FOCUS_DOWN)
            }
        }
    }

    companion object {
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
        config.logLevel = Const.LOG_DEBUG
        config.logHandler = object : Config.LogHandler {
            override fun log(level: Int, msg: String?) {
                appendLog(msg)
            }
        }
        return Connector(config)
    }
}