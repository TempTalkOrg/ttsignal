///////////////////////////////////////////////////////////////////////////////
// file : ios_bridge.h
// author : anto
//
// extern "C" API exposed to Swift via TTSignalC module (module.modulemap).
// Mirror of src/cpp/jni/JNI_SMPConnectorWrap.cpp — every method on
// Connector / Connection / Packet has an equivalent here, kept 1:1 so the
// Swift binding layer in src/swift/ matches the Java binding in src/java/.
///////////////////////////////////////////////////////////////////////////////
#ifndef TTSIGNAL_IOS_BRIDGE_H_INCLUDED__
#define TTSIGNAL_IOS_BRIDGE_H_INCLUDED__

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

///////////////////////////////////////////////////////////////////////////////
// Opaque handles
///////////////////////////////////////////////////////////////////////////////

typedef struct TTConnector*  TTConnectorRef;
typedef struct TTConnection* TTConnectionRef;
typedef struct TTPacket*     TTPacketRef;

///////////////////////////////////////////////////////////////////////////////
// Config — flat C struct, populated by Swift TTSignalConfig before
// tt_connector_create / tt_connector_create_connection. Mirrors
// src/java/.../Config.java field-by-field.
///////////////////////////////////////////////////////////////////////////////

typedef struct {
    // Connector use
    const char* hostname;
    // Server use
    int32_t     port;
    int32_t     backlog;
    int32_t     reusePort;     // bool
    int32_t     ssl;           // bool
    const char* privateKeyFile;
    const char* certificateFile;
    // Server / Connector both
    int32_t     taskThreads;
    int32_t     timerThreads;
    int32_t     idleTimeOut;
    const char* alpn;
    int32_t     maxConnections;
    int32_t     congestCtrl;   // 'B' / 'b' / 'C' / 'R' ...
    int32_t     pingOn;        // bool
    int32_t     pingInterval;
    int32_t     activeConnectionIdLimit;
    int32_t     deviceType;
    const char* cidTag;
    const char* logFile;
    int32_t     logLevel;
    int32_t     numOfSenders;
    const char* serverHost;
    const char* caCertPem;
    // Off-switch for SMPConnector's built-in auto-restart on path changes.
    // 0 (default) keeps the AppleNetworkMonitor → SMPConnection::Restart
    // pipeline live, which is what apps want — they get free QUIC
    // connection migration across cellular ↔ wifi without writing a
    // single line of code. Set to 1 for server / long-lived deployments
    // (mirrors the NAPI `config.disableAutoRestart`) where you don't
    // want NWPathMonitor jitter to bounce well-behaved connections.
    int32_t     disableAutoRestart;
} TTConfig;

///////////////////////////////////////////////////////////////////////////////
// Handler vtable — every entry maps 1:1 to IConnectionHandler.java
///////////////////////////////////////////////////////////////////////////////

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

///////////////////////////////////////////////////////////////////////////////
// Connector — see JNI_SMPConnectorWrap::SMPConnectorWrap
///////////////////////////////////////////////////////////////////////////////

// Returns NULL on failure. Caller owns the returned handle and must call
// tt_connector_destroy() once the underlying SMPConnector has fired
// OnClosed().
TTConnectorRef tt_connector_create(const TTConfig* config);

// Spawn a Connection on the engine-shared Connector. config may be the same
// as the connector config or a per-connection override (e.g. different alpn /
// idle_time_out). handler vtable callbacks are dispatched on internal worker
// threads — Swift bindings must hop to the main queue if needed. userdata is
// returned verbatim to every callback and is opaque to the bridge.
TTConnectionRef tt_connector_create_connection(TTConnectorRef connector,
                                               const TTConfig* config,
                                               const TTHandlerVTable* vtable,
                                               void* userdata);

// outBuf receives a JSON object string with keys mirroring
// SMPConnector::GetStats() (allocated_conn_size / active_conn_size /
// freed_conn_size). bufSize must be >= 256. Returns the number of bytes
// written (excluding NUL); 0 on error.
size_t tt_connector_get_stats(TTConnectorRef connector,
                              char* outBuf,
                              size_t bufSize);

// Initiate connector shutdown. The handler's on_closed will be fired
// asynchronously when the underlying engine has actually stopped.
void tt_connector_close(TTConnectorRef connector);

// Synchronous variant of tt_connector_close: kick shutdown and block
// the calling thread until the C++ engine has fully drained (i.e. the
// IConnectorHandler::OnClosed callback has fired) or `timeoutMs` ms
// have elapsed. Returns 0 (BC_R_SUCCESS) on clean close, 2
// (BC_R_TIMEDOUT) on timeout, or another BC_R_* code if the handle is
// invalid. Pass `timeoutMs <= 0` to wait forever.
//
// Use this from any teardown path that immediately wants to call
// tt_connector_destroy or rebuild a fresh connector (e.g. switching
// log level at runtime, or app shutdown that needs to free resources
// before exit). The plain async tt_connector_close otherwise races
// the destroy and can crash worker threads still touching SMPConnector
// after delete.
//
// MUST NOT be called from inside a TTHandlerVTable callback — the
// OnClosed wake-up runs on the same internal worker pool, so blocking
// it would deadlock.
int32_t tt_connector_close_sync(TTConnectorRef connector, int32_t timeoutMs);

// Final delete — only call after on_closed fired (or if create failed).
void tt_connector_destroy(TTConnectorRef connector);

///////////////////////////////////////////////////////////////////////////////
// Connection — see JNI_SMPConnectorWrap::SMPConnectionWrap
///////////////////////////////////////////////////////////////////////////////

// Initiate a QUIC connection to `url` (full URL, e.g. https://host:443/path).
// `propsJson` is a JSON object string pinned by the caller — it is forwarded
// verbatim to the server inside the SMP handshake (matches JNI's `props`).
// Returns BC_R_SUCCESS (0) on success or one of the BC_R_* error codes.
int32_t tt_connection_connect(TTConnectionRef connection,
                              const char* url,
                              const char* propsJson,
                              int32_t timeoutMs);

// Send a packet that was previously built via tt_packet_create. The bridge
// retains a reference to the underlying SMPacket until the engine has flushed
// it — the caller may destroy the TTPacketRef immediately after this returns.
int32_t tt_connection_send_packet(TTConnectionRef connection,
                                  TTPacketRef packet);

// Trigger an active QUIC migration. networkHandle == 0 keeps the existing
// interface (matches Android's restart()). On iOS networkHandle is the
// numeric ifIndex returned by nw_interface_get_index, fed straight through
// SMPConnection::Restart -> UDPSender::Restart -> setsockopt(IP_BOUND_IF).
//
// Application code does NOT normally call this — AppleNetworkMonitor invokes
// it automatically when NWPathMonitor reports a usable path change.
void tt_connection_restart(TTConnectionRef connection, int64_t networkHandle);

void tt_connection_close(TTConnectionRef connection);
void tt_connection_close_stream(TTConnectionRef connection, int32_t streamId);
void tt_connection_destroy(TTConnectionRef connection);

///////////////////////////////////////////////////////////////////////////////
// Packet — see JNI_SMPacketWrap
///////////////////////////////////////////////////////////////////////////////

// type matches Const.PTYPE_* in src/java/.../Const.java
//   PTYPE_CMD          = 1
//   PTYPE_DATA         = 2
//   PTYPE_USER_CONTROL = 3
//   PTYPE_PING         = 4
//   PTYPE_PONG         = 5
// data is copied internally — the caller can free its buffer immediately.
TTPacketRef tt_packet_create(uint8_t  type,
                             int64_t  timestamp,
                             int32_t  transId,
                             int32_t  streamId,
                             const uint8_t* data,
                             size_t   len);

void tt_packet_destroy(TTPacketRef packet);

///////////////////////////////////////////////////////////////////////////////
// Versioning — useful for sanity-checking xcframework was built from the
// same revision as the Swift binding it ships with.
///////////////////////////////////////////////////////////////////////////////

const char* tt_get_sdk_version(void);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // TTSIGNAL_IOS_BRIDGE_H_INCLUDED__
