



package org.example.android.smp;

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
                
                System.out.println("CLI连接结果(pid:" +Thread.currentThread().getId() + "): " + errorCode + ", " + message);
                if (errorCode == 0) {
                    
                    System.out.println("CLI连接成功");
                }
            }

            @Override
            public void onStreamCreated(Connection conn, Stream stream) {
                
                System.out.println("新建流: " + stream.id());
                stream.sendText("text send from connection");
            }

            @Override
            public void onStreamClosed(Connection conn, Stream stream) {
                
                System.out.println("流关闭: " + stream.id());
            }

            @Override
            public void onStreamDataAcked(Connection conn, Stream stream, long ackDelayTime, int ackedBytes, int inflightBytes) {
                
                
            }

            @Override
            public void onStreamDataSent(Connection conn, Stream stream, int transId, int size) {
                
                System.out.println("流数据发送: " + transId + ", " + size);
            }

            @Override
            public void onRecvCmd(Connection conn, long timestamp, int transId, Stream stream, byte[] buffer) {
                
                System.out.println("收到命令: " + timestamp + ", " + transId);
                String cmd = charset.decode(ByteBuffer.wrap(buffer)).toString();
                System.out.println("CLI命令内容: " + cmd);
                
            }

            @Override
            public void onRecvData(Connection conn, long timestamp, int transId, Stream stream, byte[] buffer) {
                
                System.out.println("收到数据: " + timestamp + ", " + transId);
                String data = charset.decode(ByteBuffer.wrap(buffer)).toString();
                System.out.println("CLI数据内容: " + data);
                
            }
            @Override
            public void onRestart(Connection conn, int result, String address) {
                System.out.println("CLI连接重启(pid:" +Thread.currentThread().getId() + "): " + result + ", " + address);
            }
            @Override
            public void onClosed(Connection conn, String reason) {
                System.out.println("CLI连接关闭(pid:" +Thread.currentThread().getId() + "): " + reason);
                
            }
            @Override
            public void onException(Connection conn, String errorMsg) {
                System.out.println("CLI连接异常: " + errorMsg);
                
                
            }
        });
        conn.connect("https://localhost:4433/webtransport", 
        
        
            "{}",
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
        
        
        Connector connector = new Connector(config);
        for (int i = 0; i < num_of_conns; i++) {
            createConnection(connector);
        }
        return connector;
    }

    public static void main(String[] args) {

        
        String osVersion = System.getProperty("os.version");
        System.out.println("Operating System Version: " + osVersion);

        
        String osName = System.getProperty("os.name");
        String osArch = System.getProperty("os.arch");

        System.out.println("Operating System Name: " + osName);
        System.out.println("Operating System Architecture (CPU): " + osArch);
        System.out.println("java.library.path: " + System.getProperty("java.library.path")); 
        System.out.println("user.dir: " + System.getProperty("user.dir")); 
        
        Connector connector = null;
        Scanner scanner = new Scanner(System.in);
        while (true) { 
            
            
            
            System.out.println("请输入一些内容（输入 'quit' 结束循环）:");
            String userInput = scanner.nextLine();
            String[] inputs = userInput.split(" ");
            if ("q".equalsIgnoreCase(inputs[0]) || "quit".equalsIgnoreCase(inputs[0])) {
                break; 
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

            
        }
            
        
        scanner.close(); 
        
        
        
        System.out.println("主循环已退出...");
        
    }
}