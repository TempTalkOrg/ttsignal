







#ifndef TTSIGNAL_IOS_BRIDGE_H_INCLUDED__
#define TTSIGNAL_IOS_BRIDGE_H_INCLUDED__

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif





typedef struct TTConnector*  TTConnectorRef;
typedef struct TTConnection* TTConnectionRef;
typedef struct TTPacket*     TTPacketRef;







typedef struct {
    
    const char* hostname;
    
    int32_t     port;
    int32_t     backlog;
    int32_t     reusePort;     
    int32_t     ssl;           
    const char* privateKeyFile;
    const char* certificateFile;
    
    int32_t     taskThreads;
    int32_t     timerThreads;
    int32_t     idleTimeOut;
    const char* alpn;
    int32_t     maxConnections;
    int32_t     congestCtrl;   
    int32_t     pingOn;        
    int32_t     pingInterval;
    int32_t     activeConnectionIdLimit;
    int32_t     deviceType;
    const char* cidTag;
    const char* logFile;
    int32_t     logLevel;
    int32_t     numOfSenders;
    const char* serverHost;
    const char* caCertPem;
    
    
    
    
    
    
    
    const char* proxyUrl;
    const char* proxyHost;
    int32_t     proxyPort;   
    const char* proxySni;
    
    
    const char* proxyCaCertPem;
    
    
    const char* spkiPin;
    
    
    
    
    
    
    
    int32_t     disableAutoRestart;
    
    
    
    
    
    
    
    
    
    int32_t     bypassVpn;
    
    
    
    
    
    
    
    
    
    
    int32_t     vpnPolicy;
} TTConfig;





typedef struct {
    void (*on_connect_result)(void* userdata,
                              int32_t error,
                              const char* message);
    void (*on_stream_created)(void* userdata,
                              int32_t streamId);
    void (*on_stream_closed)(void* userdata,
                             int32_t streamId);
    void (*on_stream_data_acked)(void* userdata,
                                 int32_t streamId,
                                 int64_t ackDelayUs,
                                 int32_t ackedBytes,
                                 int32_t inflightBytes);
    void (*on_stream_data_sent)(void* userdata,
                                int32_t streamId,
                                int32_t transId,
                                int32_t size);
    void (*on_recv_cmd)(void* userdata,
                        int64_t timestamp,
                        int32_t transId,
                        int32_t streamId,
                        const uint8_t* data,
                        size_t len);
    void (*on_recv_data)(void* userdata,
                         int64_t timestamp,
                         int32_t transId,
                         int32_t streamId,
                         const uint8_t* data,
                         size_t len);
    void (*on_restart)(void* userdata,
                       int32_t result,
                       const char* localAddr);
    void (*on_closed)(void* userdata,
                      const char* reason);
    void (*on_exception)(void* userdata,
                         const char* errMsg);
} TTHandlerVTable;








TTConnectorRef tt_connector_create(const TTConfig* config);






TTConnectionRef tt_connector_create_connection(TTConnectorRef connector,
                                               const TTConfig* config,
                                               const TTHandlerVTable* vtable,
                                               void* userdata);





size_t tt_connector_get_stats(TTConnectorRef connector,
                              char* outBuf,
                              size_t bufSize);



void tt_connector_close(TTConnectorRef connector);


















int32_t tt_connector_close_sync(TTConnectorRef connector, int32_t timeoutMs);


void tt_connector_destroy(TTConnectorRef connector);









int32_t tt_connection_connect(TTConnectionRef connection,
                              const char* url,
                              const char* propsJson,
                              int32_t timeoutMs);




int32_t tt_connection_send_packet(TTConnectionRef connection,
                                  TTPacketRef packet);








void tt_connection_restart(TTConnectionRef connection, int64_t networkHandle);

void tt_connection_close(TTConnectionRef connection);
void tt_connection_close_stream(TTConnectionRef connection, int32_t streamId);
void tt_connection_destroy(TTConnectionRef connection);












TTPacketRef tt_packet_create(uint8_t  type,
                             int64_t  timestamp,
                             int32_t  transId,
                             int32_t  streamId,
                             const uint8_t* data,
                             size_t   len);

void tt_packet_destroy(TTPacketRef packet);






const char* tt_get_sdk_version(void);





















typedef struct TTHttpConnector* TTHttpConnectorRef;


typedef void (*TTCloseDone)(void* userdata);

typedef struct {
    
    
    int32_t     vpnPolicy;
    const char* caCerts;            
    const char* spkiPin;
    int32_t     insecureSkipVerify; 
    const char* dnsServers;
    uint32_t    dnsTimeoutMs;
    uint32_t    drainTimeoutMs;     
    int32_t     logLevel;           
    size_t      maxResponseBytes;   
} TTHttpConfig;

typedef struct {
    const char* method;             
    const char* url;                
    const char* const* headerNames; 
    const char* const* headerValues;
    size_t      headerCount;
    const uint8_t* body;
    size_t      bodyLen;
    uint32_t    timeoutMs;          
    const char* resolvedIp;         
} TTHttpRequest;

typedef struct {
    int32_t     status;
    
    
    const char* reason;
    
    
    
    const char* const* headerNames;
    const char* const* headerValues;
    size_t      headerCount;
    const uint8_t* body;
    size_t      bodyLen;
    
    const char* peerIp;
    uint32_t    boundIfIndex;       
    const char* pinMethod;          
} TTHttpResponse;






typedef void (*TTHttpCompletion)(void* userdata,
                                 int32_t error,
                                 const char* errName,
                                 const char* errMessage,
                                 const TTHttpResponse* resp);







const char* tt_result_name(int32_t result);










const char* tt_last_sync_error(void);


TTHttpConnectorRef tt_http_connector_create(const TTHttpConfig* config);


int32_t tt_http_request(TTHttpConnectorRef connector,
                        const TTHttpRequest* req,
                        TTHttpCompletion completion,
                        void* userdata);

void tt_http_connector_close(TTHttpConnectorRef connector,
                             TTCloseDone done, void* userdata);
void tt_http_connector_destroy(TTHttpConnectorRef connector);





typedef struct TTWSConnector*  TTWSConnectorRef;
typedef struct TTWSConnection* TTWSConnectionRef;

typedef struct {
    
    int32_t     vpnPolicy;          
    const char* caCerts;
    const char* spkiPin;
    int32_t     insecureSkipVerify;
    const char* dnsServers;
    uint32_t    dnsTimeoutMs;
    uint32_t    drainTimeoutMs;
    int32_t     logLevel;
    
    uint32_t    connectTimeoutMs;
    uint32_t    pingIntervalMs;
    uint32_t    idleTimeoutMs;
    size_t      maxFrameBytes;
    const char* clientCertFile;
    const char* clientKeyFile;
    const char* clientKeyPassword;
} TTWSConfig;















typedef struct {
    void (*on_connect_result)(void* ud, int32_t error, const char* message);
    void (*on_text)     (void* ud, const char* text, size_t len);
    void (*on_data)     (void* ud, const uint8_t* data, size_t len);
    void (*on_closed)   (void* ud, const char* reason);
    void (*on_exception)(void* ud, const char* errMsg);
    void (*on_release)  (void* ud);
} TTWSHandlerVTable;

TTWSConnectorRef tt_ws_connector_create(const TTWSConfig* config);

TTWSConnectionRef tt_ws_connector_create_connection(TTWSConnectorRef connector,
                        const TTWSHandlerVTable* vtable, void* userdata);
void tt_ws_connector_close(TTWSConnectorRef connector,
                           TTCloseDone done, void* userdata);
void tt_ws_connector_destroy(TTWSConnectorRef connector);



int32_t tt_ws_connection_connect(TTWSConnectionRef conn,
                                 const char* url, uint32_t timeoutMs);


int32_t tt_ws_connection_set_request_header(TTWSConnectionRef conn,
                                            const char* name, const char* value);


int32_t tt_ws_connection_send_text(TTWSConnectionRef conn,
                                   const char* text, size_t len);
int32_t tt_ws_connection_send_data(TTWSConnectionRef conn,
                                   const uint8_t* data, size_t len);
int32_t tt_ws_connection_send_ping(TTWSConnectionRef conn);

uint32_t tt_ws_connection_bound_if(TTWSConnectionRef conn);
void tt_ws_connection_close(TTWSConnectionRef conn);
void tt_ws_connection_destroy(TTWSConnectionRef conn);

#ifdef __cplusplus
} 
#endif

#endif 
