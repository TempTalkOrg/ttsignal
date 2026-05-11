///////////////////////////////////////////////////////////////////////////////
// file : ios_logger.mm
// author : anto
///////////////////////////////////////////////////////////////////////////////

#include "ios_logger.h"

#import <Foundation/Foundation.h>
#import <os/log.h>

#include <atomic>
#include <mutex>

namespace {

std::mutex                          g_lock;
TTLogCallback                       g_cb       = nullptr;
void*                               g_userdata = nullptr;

os_log_t SubsystemLog() {
    static os_log_t logger = ^os_log_t {
        return os_log_create("org.difft.ttsignal", "core");
    }();
    return logger;
}

// Map TTSignal public log level → OSLog type. The `level` value passed to
// IConnectorHandler::OnLog by SMPConnector::log_callback has already been
// run through BCLogLevelToLogLevel() (see SMPConnector.cpp), so it follows
// Const.LOG_* / Utils.h LOG_LEVEL_* (1..5):
//   1 = LOG_DEBUG, 2 = LOG_INFO, 3 = LOG_WARN, 4 = LOG_ERROR, 5 = LOG_FATAL.
// OSLog has no warn level, so warn folds to ERROR (we still want it noticed).
os_log_type_t MapLevel(int32_t lvl) {
    switch (lvl) {
        case 1: return OS_LOG_TYPE_DEBUG;   // LOG_DEBUG
        case 2: return OS_LOG_TYPE_INFO;    // LOG_INFO
        case 3: return OS_LOG_TYPE_ERROR;   // LOG_WARN  (OSLog has no warn)
        case 4: return OS_LOG_TYPE_ERROR;   // LOG_ERROR
        case 5: return OS_LOG_TYPE_FAULT;   // LOG_FATAL
        default: return OS_LOG_TYPE_DEFAULT;
    }
}

} // namespace

extern "C" {

void tt_logger_set_callback(TTLogCallback cb, void* userdata)
{
    std::lock_guard<std::mutex> g(g_lock);
    g_cb = cb;
    g_userdata = userdata;
}

void ios_logger_dispatch(int32_t level, const char* msg)
{
    if (!msg) msg = "";

    // Snapshot the callback under the lock so we can release it before
    // invoking arbitrary Swift code that might allocate / log recursively.
    TTLogCallback cb;
    void* ud;
    {
        std::lock_guard<std::mutex> g(g_lock);
        cb = g_cb;
        ud = g_userdata;
    }

    if (cb) {
        // Swift sink installed — caller owns log delivery. Suppress the
        // OSLog backend entirely, otherwise Console.app gets a duplicate
        // of every line the host app is already rendering / persisting
        // through its own pipeline.
        cb(ud, level, msg);
    } else {
        // No Swift sink — fall back to the default OSLog backend.
        // %{public}s keeps the message visible in Console.app even on
        // signed release builds (no automatic redaction).
        os_log_with_type(SubsystemLog(), MapLevel(level), "%{public}s", msg);
    }
}

} // extern "C"
