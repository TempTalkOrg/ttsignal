///////////////////////////////////////////////////////////////////////////////
// file : ios_logger.h
// author : anto
//
// Logging glue between the SMP core (BCLog / LogQ) and iOS. By default
// every log line is forwarded to OSLog under subsystem "org.difft.ttsignal"
// and category "core". When Swift installs a callback via
// tt_logger_set_callback, OSLog is suppressed and **only** the callback
// receives log lines — host apps that already have their own logging
// pipeline (file rotation, remote upload, in-app viewer, etc.) shouldn't
// also get duplicate spam in Console.app. Removing the callback (passing
// NULL) re-enables the OSLog backend.
///////////////////////////////////////////////////////////////////////////////
#ifndef TTSIGNAL_IOS_LOGGER_H_INCLUDED__
#define TTSIGNAL_IOS_LOGGER_H_INCLUDED__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// `level` follows the *public* TTSignal log level scheme — same as
// Const.LOG_* (src/java/.../Const.java) and Utils.h LOG_LEVEL_*:
//   1 = LOG_DEBUG
//   2 = LOG_INFO
//   3 = LOG_WARN
//   4 = LOG_ERROR
//   5 = LOG_FATAL
// This is the value SMPConnector::log_callback hands to
// IConnectorHandler::OnLog(level, msg) AFTER going through
// BCLogLevelToLogLevel() (see SMPConnector.cpp), so it is NOT the raw
// BCLog level (_FATAL_=0, _ERROR_=1, _WARN_=2, _INFO_=3, _DEBUG_=4,
// _FINEST_=6) — the conversion has already happened by the time we
// reach the iOS bridge.
typedef void (*TTLogCallback)(void* userdata, int32_t level, const char* msg);

// Install a Swift-side log sink. Pass NULL to remove. Replaces any
// previous callback. Thread-safe: callbacks may fire from any thread.
void tt_logger_set_callback(TTLogCallback cb, void* userdata);

// Internal — invoked by the bridge's IOSConnectorHandlerProxy::OnLog and
// nowhere else. Not part of the Swift module surface but exposed here so
// ios_bridge.mm can call it from the C++ side.
void ios_logger_dispatch(int32_t level, const char* msg);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // TTSIGNAL_IOS_LOGGER_H_INCLUDED__
