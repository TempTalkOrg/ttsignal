

















#ifndef TTSIGNAL_INETWORK_PATH_MONITOR_H_INCLUDED__
#define TTSIGNAL_INETWORK_PATH_MONITOR_H_INCLUDED__

#include <stdint.h>





#include "VpnPolicy.h"

#ifdef __cplusplus
extern "C" {
#endif



typedef struct TTNetworkMonitor* TTNetworkMonitorRef;












typedef void (*TTPathChangeCallback)(void* userdata,
                                     int64_t newIfIndex,
                                     const char* pathDesc);






typedef void (*TTPathLogCallback)(void* userdata, const char* line);






typedef struct TTNetworkMonitorOptions {
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    int bypassVpn;
    
    
    
    
    
    
    
    TTPathLogCallback rawLogFn;
    void* rawLogCtx;
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    int vpnPolicy;
} TTNetworkMonitorOptions;















TTNetworkMonitorRef tt_netmon_start(const TTNetworkMonitorOptions* options,
                                    TTPathChangeCallback cb,
                                    void* userdata);



void tt_netmon_stop(TTNetworkMonitorRef ref);







int64_t tt_netmon_query_default_ifindex(void);












int64_t tt_netmon_query_default_ifindex_ex(int vpnPolicy);

#ifdef __cplusplus
} 
#endif

#endif 
