



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
        return os_log_create("app.example.ttsignal", "core");
    }();
    return logger;
}







os_log_type_t MapLevel(int32_t lvl) {
    switch (lvl) {
        case 1: return OS_LOG_TYPE_DEBUG;   
        case 2: return OS_LOG_TYPE_INFO;    
        case 3: return OS_LOG_TYPE_ERROR;   
        case 4: return OS_LOG_TYPE_ERROR;   
        case 5: return OS_LOG_TYPE_FAULT;   
        default: return OS_LOG_TYPE_DEFAULT;
    }
}

} 

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

    
    
    TTLogCallback cb;
    void* ud;
    {
        std::lock_guard<std::mutex> g(g_lock);
        cb = g_cb;
        ud = g_userdata;
    }

    if (cb) {
        
        
        
        
        cb(ud, level, msg);
    } else {
        
        
        
        os_log_with_type(SubsystemLog(), MapLevel(level), "%{public}s", msg);
    }
}

} 
