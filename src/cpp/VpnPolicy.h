
















#ifndef TT_VPN_POLICY_H
#define TT_VPN_POLICY_H

#include <stddef.h>
#include <stdint.h>




#include "NetworkRouteLookup.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif



typedef enum {
    TT_VPN_POLICY_UNSET           = -1,  
    TT_VPN_POLICY_OS              = 0,
    TT_VPN_POLICY_PREFER_PHYSICAL = 1,
    TT_VPN_POLICY_FORCE_PHYSICAL  = 2,
} TTVpnPolicy;









TTVpnPolicy tt_vpn_policy_platform_default(void);




TTVpnPolicy tt_vpn_policy_from_string(const char* s);


const char* tt_vpn_policy_to_string(TTVpnPolicy p);















TTVpnPolicy tt_vpn_policy_resolve(TTVpnPolicy explicitPolicy,
                                  int hasBypassVpn,
                                  int bypassVpn,
                                  TTVpnPolicy platformDefault,
                                  int* outBothGiven);














#define TT_PEER_CLASS_GLOBAL    ((uint32_t)0u)



#define TT_PEER_CLASS_FAKE_IP   ((uint32_t)0x1u)



#define TT_PEER_CLASS_PRIVATE   ((uint32_t)0x2u)




uint32_t tt_peer_address_class(const struct sockaddr* dst, size_t dst_len);


typedef enum {
    TT_PIN_VERDICT_KEEP  = 0,  
    TT_PIN_VERDICT_UNPIN = 1,  
} TTPinVerdict;



typedef enum {
    TT_PIN_REASON_OK = 0,
    
    TT_PIN_REASON_SCOPED_MISMATCH,
    
    TT_PIN_REASON_SCOPED_NO_ROUTE,
    
    
    TT_PIN_REASON_BLACKHOLE_RISK,
    
    TT_PIN_REASON_GLOBAL_MISMATCH,
} TTPinRecheckReason;






















































TTPinVerdict tt_vpn_pin_recheck(TTVpnPolicy policy,
                                uint32_t boundIfIndex,
                                uint32_t scopedIfIndex,
                                uint32_t globalIfIndex,
                                uint32_t peerClass,
                                TTPinRecheckReason* outReason);


const char* tt_vpn_pin_reason_to_string(TTPinRecheckReason r);

#ifdef __cplusplus
}
#endif

#endif 




