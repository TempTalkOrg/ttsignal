



package org.example.android.smp;


public class Config {
     public interface LogHandler {
         public void log(int level, String msg);
     }

    
    public String hostname = "localhost";
    
    public int port = 8003;
    public int backlog = 1000;
    public boolean reusePort = false;
    public boolean ssl = false;
    public String privateKeyFile = "";
    public String certificateFile = "";
    
    public int taskThreads = 16;
    public int timerThreads = 4;
    
    public int idleTimeOut = 20000;
    public String alpn = "ttsignal"; 
    public int maxConnections = 1000;
    public int congestCtrl = 'B';
    
    public boolean pingOn = false;
    
    public int pingInterval = 10000;
    
    public int activeConnectionIdLimit = 1000;
    
    public int deviceType = 0;
    
    public String cidTag = "";
    public String logFile = "";
    public LogHandler logHandler = null;
    
    public int logLevel = 0;
    
    public int numOfSenders = 1;
    
    public String serverHost = "";
    
    public String caCertPem = "";

    public Config() {
        
    }
}
