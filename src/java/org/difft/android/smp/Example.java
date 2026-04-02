///////////////////////////////////////////////////////////////////////////////
// file : Example.java
// author : antoniozhou
///////////////////////////////////////////////////////////////////////////////

package org.difft.android.smp;

import java.nio.ByteBuffer;
import java.nio.charset.Charset;
import java.nio.charset.StandardCharsets;
import java.util.Scanner;

public class Example {

    private static void createConnection(Connector connector)
    {
        Config config = new Config();
        config.idleTimeOut = 20000;
        config.hostname = "localhost";
        config.port = 8003;
        config.maxConnections = 1000;
        config.congestCtrl = 'B';
        config.pingOn = true;
        config.alpn = "h3";
        
        Charset charset = StandardCharsets.UTF_8;
        Connection conn = connector.createConnection(config, new IConnectionHandler() {
            @Override
            public void onConnectResult(Connection conn, int errorCode, String message) {
                // 连接失败时的处理逻辑
                System.out.println("CLI连接结果(pid:" +Thread.currentThread().getId() + "): " + errorCode + ", " + message);
                if (errorCode == 0) {
                    // 连接成功时的处理逻辑
                    System.out.println("CLI连接成功");
                }
            }

            @Override
            public void onStreamCreated(Connection conn, Stream stream) {
                // 新建流时的处理逻辑
                System.out.println("新建流: " + stream.id());
                stream.sendText("text send from connection");
            }

            @Override
            public void onStreamClosed(Connection conn, Stream stream) {
                // 流关闭时的处理逻辑
                System.out.println("流关闭: " + stream.id());
            }

            @Override
            public void onRecvCmd(Connection conn, long timestamp, int transId, Stream stream, byte[] buffer) {
                // 收到命令时的处理逻辑
                System.out.println("收到命令: " + timestamp + ", " + transId);
                String cmd = charset.decode(ByteBuffer.wrap(buffer)).toString();
                System.out.println("CLI命令内容: " + cmd);
                // 可以根据需要进行进一步处理
            }

            @Override
            public void onRecvData(Connection conn, long timestamp, int transId, Stream stream, byte[] buffer) {
                // 收到数据时的处理逻辑
                System.out.println("收到数据: " + timestamp + ", " + transId);
                String data = charset.decode(ByteBuffer.wrap(buffer)).toString();
                System.out.println("CLI数据内容: " + data);
                // 可以根据需要进行进一步处理
            }
            @Override
            public void onRestart(Connection conn, int result, String address) {
                System.out.println("CLI连接重启(pid:" +Thread.currentThread().getId() + "): " + result + ", " + address);
            }
            @Override
            public void onClosed(Connection conn, String reason) {
                System.out.println("CLI连接关闭(pid:" +Thread.currentThread().getId() + "): " + reason);
                // 可以根据需要进行进一步处理
            }
            @Override
            public void onException(Connection conn, String errorMsg) {
                System.out.println("CLI连接异常: " + errorMsg);
                // 可以根据需要进行进一步处理
                
            }
        });
        conn.connect("https://localhost:4433/webtransport", 
        //     "{\"Authorization\":\"Bearer test_token\"}");
        // conn.connect("https://192.168.1.3:7880/rtc", 
            "{\"Authorization\":\"Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJleHAiOjE4NDc1MjQ0MzQsImlzcyI6ImRldmtleSIsIm5hbWUiOiJ6azEiLCJuYmYiOjE3NjExMjQ0MzQsInN1YiI6InprMSIsInZpZGVvIjp7InJvb20iOiJyb29tMSIsInJvb21Kb2luIjp0cnVlfX0.cU5ujtKmZLrvqjKr1zyFhb4PAlSSNdaTP0TvWpE17kA\"}",
                10000);
        
    }

    public static Connector test_connector(int num_of_conns){
        Config config = new Config();
        config.taskThreads = 16;
        config.timerThreads = 4;
        config.idleTimeOut = 20000;
        config.alpn = "ttsignal";
        config.hostname = "localhost";
        config.port = 4433;
        config.maxConnections = 1000;
        config.congestCtrl = 'B';
        config.pingOn = true;
        config.logFile = "ttclient.log";
        config.logLevel = Const.LOG_ERROR;
        
        // 假设这里有一个connector对象，它需要使用config对象
        Connector connector = new Connector(config);
        for (int i = 0; i < num_of_conns; i++) {
            createConnection(connector);
        }
        return connector;
    }

    public static void main(String[] args) {

        // 获取操作系统版本
        String osVersion = System.getProperty("os.version");
        System.out.println("Operating System Version: " + osVersion);

        // 获取操作系统名称和架构（包括CPU架构）
        String osName = System.getProperty("os.name");
        String osArch = System.getProperty("os.arch");

        System.out.println("Operating System Name: " + osName);
        System.out.println("Operating System Architecture (CPU): " + osArch);
        System.out.println("java.library.path: " + System.getProperty("java.library.path")); 
        System.out.println("user.dir: " + System.getProperty("user.dir")); 
        
        Connector connector = null;
        Scanner scanner = new Scanner(System.in);
        while (true) { // 这将创建一个无限循环
            // 主循环体内的代码会不断重复执行
            
            // 例如：读取用户输入并处理
            System.out.println("请输入一些内容（输入 'quit' 结束循环）:");
            String userInput = scanner.nextLine();
            String[] inputs = userInput.split(" ");
            if ("q".equalsIgnoreCase(inputs[0]) || "quit".equalsIgnoreCase(inputs[0])) {
                break; // 当用户输入 "quit" 时，跳出循环
            } 
            else if ("c".equalsIgnoreCase(inputs[0])) {
                connector = test_connector(1);
            }
            else if ("c1k".equalsIgnoreCase(inputs[0])) {
                connector = test_connector(1000);
            }
            else if ("c2k".equalsIgnoreCase(inputs[0])) {
                connector = test_connector(2000);
            }
            else if ("c3k".equalsIgnoreCase(inputs[0])) {
                connector = test_connector(3000);
            }
            else if ("c4k".equalsIgnoreCase(inputs[0])) {
                connector = test_connector(4000);
            }
            else if ("c5k".equalsIgnoreCase(inputs[0])) {
                connector = test_connector(5000);
            }
            else if ("c10k".equalsIgnoreCase(inputs[0])) {
                connector = test_connector(10000);
            }
            else if ("gc".equalsIgnoreCase(inputs[0])) {
                System.gc();
                System.out.println("GC执行完毕");
            }
            else if ("cc".equalsIgnoreCase(inputs[0])) {
                System.out.println("Close connector");
                if (connector != null){
                    connector.close();
                    connector = null;
                }
            }
            else if ("stat".equalsIgnoreCase(inputs[0])) {
                System.out.println("stats connections :");
                if (connector != null){
                    System.out.println("connector.getStats() : " + connector.getStats());
                }
            }

            // 处理用户输入或进行其他操作...
        }
            
        // 可以在这里放置定期更新、状态检查或其他持续运行的任务
        scanner.close(); // 确保在循环外关闭扫描器
        
        // 当循环结束（如上例所示，当用户输入 "quit" 时），
        // 这里的代码将会被执行
        System.out.println("主循环已退出...");
        // 接下来可以执行其他清理操作或退出程序
    }
}