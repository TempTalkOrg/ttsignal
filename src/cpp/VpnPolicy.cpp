



#include "VpnPolicy.h"

#include <string.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#endif

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

extern "C" {

TTVpnPolicy tt_vpn_policy_platform_default(void)
{
    
    
#if defined(__APPLE__) && TARGET_OS_IPHONE && !TARGET_OS_MACCATALYST
    return TT_VPN_POLICY_OS;
#else
    return TT_VPN_POLICY_PREFER_PHYSICAL;
#endif
}

TTVpnPolicy tt_vpn_policy_from_string(const char* s)
{
    if (s == NULL || s[0] == '\0') {
        return TT_VPN_POLICY_UNSET;
    }
    if (strcmp(s, "os") == 0) {
        return TT_VPN_POLICY_OS;
    }
    if (strcmp(s, "prefer-physical") == 0) {
        return TT_VPN_POLICY_PREFER_PHYSICAL;
    }
    if (strcmp(s, "force-physical") == 0) {
        return TT_VPN_POLICY_FORCE_PHYSICAL;
    }
    return TT_VPN_POLICY_UNSET;
}

const char* tt_vpn_policy_to_string(TTVpnPolicy p)
{
    switch (p) {
        case TT_VPN_POLICY_OS:              return "os";
        case TT_VPN_POLICY_PREFER_PHYSICAL: return "prefer-physical";
        case TT_VPN_POLICY_FORCE_PHYSICAL:  return "force-physical";
        case TT_VPN_POLICY_UNSET:           return "unset";
        default:                            return "invalid";
    }
}

TTVpnPolicy tt_vpn_policy_resolve(TTVpnPolicy explicitPolicy,
                                  int hasBypassVpn,
                                  int bypassVpn,
                                  TTVpnPolicy platformDefault,
                                  int* outBothGiven)
{
    const int explicitGiven = (explicitPolicy == TT_VPN_POLICY_OS ||
                               explicitPolicy == TT_VPN_POLICY_PREFER_PHYSICAL ||
                               explicitPolicy == TT_VPN_POLICY_FORCE_PHYSICAL);

    if (outBothGiven) {
        *outBothGiven = (explicitGiven && hasBypassVpn) ? 1 : 0;
    }
    if (explicitGiven) {
        return explicitPolicy;
    }
    if (hasBypassVpn) {
        return bypassVpn ? TT_VPN_POLICY_PREFER_PHYSICAL : TT_VPN_POLICY_OS;
    }
    return platformDefault;
}






static uint32_t _classify_v4(uint32_t ip)
{
    uint32_t cls = TT_PEER_CLASS_GLOBAL;

    
    
    
    if ((ip & 0xFFFE0000u) == 0xC6120000u)  
    {
        cls |= TT_PEER_CLASS_FAKE_IP | TT_PEER_CLASS_PRIVATE;
        return cls;
    }
    if ((ip & 0xFF000000u) == 0x1C000000u)  
    {
        cls |= TT_PEER_CLASS_FAKE_IP;
        return cls;
    }

    
    
    if ((ip & 0xFF000000u) == 0x00000000u ||  
        (ip & 0xFF000000u) == 0x0A000000u ||  
        (ip & 0xFFC00000u) == 0x64400000u ||  
        (ip & 0xFF000000u) == 0x7F000000u ||  
        (ip & 0xFFFF0000u) == 0xA9FE0000u ||  
        (ip & 0xFFF00000u) == 0xAC100000u ||  
        (ip & 0xFFFFFF00u) == 0xC0000000u ||  
        (ip & 0xFFFFFF00u) == 0xC0000200u ||  
        (ip & 0xFFFF0000u) == 0xC0A80000u ||  
        (ip & 0xFFFFFF00u) == 0xC6336400u ||  
        (ip & 0xFFFFFF00u) == 0xCB007100u ||  
        (ip & 0xF0000000u) == 0xE0000000u ||  
        (ip & 0xF0000000u) == 0xF0000000u)    
    {
        cls |= TT_PEER_CLASS_PRIVATE;
    }
    return cls;
}

uint32_t tt_peer_address_class(const struct sockaddr* dst, size_t dst_len)
{
    if (dst == NULL)
    {
        return TT_PEER_CLASS_GLOBAL;
    }

    if (dst->sa_family == AF_INET)
    {
        if (dst_len < sizeof(struct sockaddr_in))
        {
            return TT_PEER_CLASS_GLOBAL;
        }
        const struct sockaddr_in* sin = (const struct sockaddr_in*)dst;
        return _classify_v4(ntohl((uint32_t)sin->sin_addr.s_addr));
    }

    if (dst->sa_family == AF_INET6)
    {
        if (dst_len < sizeof(struct sockaddr_in6))
        {
            return TT_PEER_CLASS_GLOBAL;
        }
        const struct sockaddr_in6* sin6 = (const struct sockaddr_in6*)dst;
        const unsigned char* a = (const unsigned char*)&sin6->sin6_addr;

        
        
        
        static const unsigned char kV4MappedPrefix[12] = {
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF
        };
        if (memcmp(a, kV4MappedPrefix, 12) == 0)
        {
            uint32_t v4 = ((uint32_t)a[12] << 24) | ((uint32_t)a[13] << 16) |
                          ((uint32_t)a[14] << 8)  |  (uint32_t)a[15];
            return _classify_v4(v4);
        }

        uint32_t cls = TT_PEER_CLASS_GLOBAL;
        if ((a[0] & 0xFE) == 0xFC)                       
        {
            cls |= TT_PEER_CLASS_PRIVATE;
        }
        else if (a[0] == 0xFE && (a[1] & 0xC0) == 0x80)  
        {
            cls |= TT_PEER_CLASS_PRIVATE;
        }
        else if (a[0] == 0xFF)                           
        {
            cls |= TT_PEER_CLASS_PRIVATE;
        }
        else if (a[0] == 0x20 && a[1] == 0x01 &&
                 a[2] == 0x0D && a[3] == 0xB8)           
        {
            cls |= TT_PEER_CLASS_PRIVATE;
        }
        else
        {
            
            int allZeroHigh = 1;
            for (int i = 0; i < 15; ++i)
            {
                if (a[i] != 0) { allZeroHigh = 0; break; }
            }
            if (allZeroHigh && (a[15] == 0 || a[15] == 1))
            {
                cls |= TT_PEER_CLASS_PRIVATE;
            }
        }
        return cls;
    }

    return TT_PEER_CLASS_GLOBAL;
}





const char* tt_vpn_pin_reason_to_string(TTPinRecheckReason r)
{
    switch (r) {
        case TT_PIN_REASON_OK:              return "ok";
        case TT_PIN_REASON_SCOPED_MISMATCH: return "scoped-mismatch";
        case TT_PIN_REASON_SCOPED_NO_ROUTE: return "scoped-no-route";
        case TT_PIN_REASON_BLACKHOLE_RISK:  return "blackhole-risk";
        case TT_PIN_REASON_GLOBAL_MISMATCH: return "global-mismatch";
        default:                            return "invalid";
    }
}

TTPinVerdict tt_vpn_pin_recheck(TTVpnPolicy policy,
                                uint32_t boundIfIndex,
                                uint32_t scopedIfIndex,
                                uint32_t globalIfIndex,
                                uint32_t peerClass,
                                TTPinRecheckReason* outReason)
{
    TTPinRecheckReason reason = TT_PIN_REASON_OK;
    TTPinVerdict verdict = TT_PIN_VERDICT_KEEP;

    
    if (boundIfIndex == 0)
    {
        goto done;
    }

    
    
    if (policy == TT_VPN_POLICY_FORCE_PHYSICAL)
    {
        goto done;
    }

    if (policy == TT_VPN_POLICY_PREFER_PHYSICAL)
    {
        
        
        
        if (scopedIfIndex == TT_ROUTE_IFINDEX_NO_ROUTE)
        {
            reason  = TT_PIN_REASON_SCOPED_NO_ROUTE;
            verdict = TT_PIN_VERDICT_UNPIN;
            goto done;
        }
        if (scopedIfIndex != TT_ROUTE_IFINDEX_UNKNOWN &&
            scopedIfIndex != boundIfIndex)
        {
            reason  = TT_PIN_REASON_SCOPED_MISMATCH;
            verdict = TT_PIN_VERDICT_UNPIN;
            goto done;
        }

        
        
        
        
        if (peerClass != TT_PEER_CLASS_GLOBAL &&
            (globalIfIndex == TT_ROUTE_IFINDEX_UNKNOWN ||
             globalIfIndex != boundIfIndex))
        {
            reason  = TT_PIN_REASON_BLACKHOLE_RISK;
            verdict = TT_PIN_VERDICT_UNPIN;
            goto done;
        }
        goto done;
    }

    
    
    if (globalIfIndex != TT_ROUTE_IFINDEX_UNKNOWN &&
        globalIfIndex != boundIfIndex)
    {
        reason  = TT_PIN_REASON_GLOBAL_MISMATCH;
        verdict = TT_PIN_VERDICT_UNPIN;
        goto done;
    }
    
    
    
    
    
    
    
    
    if (globalIfIndex == TT_ROUTE_IFINDEX_UNKNOWN &&
        peerClass != TT_PEER_CLASS_GLOBAL)
    {
        reason  = TT_PIN_REASON_BLACKHOLE_RISK;
        verdict = TT_PIN_VERDICT_UNPIN;
        goto done;
    }

done:
    if (outReason != NULL)
    {
        *outReason = reason;
    }
    return verdict;
}

} 




