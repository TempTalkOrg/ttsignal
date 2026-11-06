


#include "VpnPolicy.h"

#include <cstdio>
#include <cstring>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

static void test_from_string()
{
    CHECK(tt_vpn_policy_from_string("os") == TT_VPN_POLICY_OS);
    CHECK(tt_vpn_policy_from_string("prefer-physical") == TT_VPN_POLICY_PREFER_PHYSICAL);
    CHECK(tt_vpn_policy_from_string("force-physical") == TT_VPN_POLICY_FORCE_PHYSICAL);
    
    CHECK(tt_vpn_policy_from_string("physical") == TT_VPN_POLICY_UNSET);
    CHECK(tt_vpn_policy_from_string("") == TT_VPN_POLICY_UNSET);
    CHECK(tt_vpn_policy_from_string(NULL) == TT_VPN_POLICY_UNSET);
    
    CHECK(tt_vpn_policy_from_string("OS") == TT_VPN_POLICY_UNSET);
}

static void test_to_string()
{
    CHECK(strcmp(tt_vpn_policy_to_string(TT_VPN_POLICY_OS), "os") == 0);
    CHECK(strcmp(tt_vpn_policy_to_string(TT_VPN_POLICY_PREFER_PHYSICAL), "prefer-physical") == 0);
    CHECK(strcmp(tt_vpn_policy_to_string(TT_VPN_POLICY_FORCE_PHYSICAL), "force-physical") == 0);
    CHECK(strcmp(tt_vpn_policy_to_string(TT_VPN_POLICY_UNSET), "unset") == 0);
    
    CHECK(tt_vpn_policy_to_string((TTVpnPolicy)99) != NULL);
}


static void test_resolve_matrix()
{
    const TTVpnPolicy kDefault = TT_VPN_POLICY_PREFER_PHYSICAL;
    int both = -1;

    
    CHECK(tt_vpn_policy_resolve(TT_VPN_POLICY_UNSET, 0, 0, kDefault, &both) == kDefault);
    CHECK(both == 0);
    CHECK(tt_vpn_policy_resolve(TT_VPN_POLICY_UNSET, 1, 0, kDefault, &both) == TT_VPN_POLICY_OS);
    CHECK(both == 0);
    CHECK(tt_vpn_policy_resolve(TT_VPN_POLICY_UNSET, 1, 1, kDefault, &both) == TT_VPN_POLICY_PREFER_PHYSICAL);
    CHECK(both == 0);

    
    const TTVpnPolicy kExplicit[] = {
        TT_VPN_POLICY_OS, TT_VPN_POLICY_PREFER_PHYSICAL, TT_VPN_POLICY_FORCE_PHYSICAL,
    };
    for (int i = 0; i < 3; i++) {
        CHECK(tt_vpn_policy_resolve(kExplicit[i], 0, 0, kDefault, &both) == kExplicit[i]);
        CHECK(both == 0);
        CHECK(tt_vpn_policy_resolve(kExplicit[i], 1, 0, kDefault, &both) == kExplicit[i]);
        CHECK(both == 1);
        CHECK(tt_vpn_policy_resolve(kExplicit[i], 1, 1, kDefault, &both) == kExplicit[i]);
        CHECK(both == 1);
    }

    
    CHECK(tt_vpn_policy_resolve(TT_VPN_POLICY_UNSET, 0, 0, TT_VPN_POLICY_OS, &both) == TT_VPN_POLICY_OS);

    
    CHECK(tt_vpn_policy_resolve(TT_VPN_POLICY_OS, 1, 1, kDefault, NULL) == TT_VPN_POLICY_OS);
}


static void test_platform_default()
{
#if defined(__APPLE__) && TARGET_OS_IPHONE && !TARGET_OS_MACCATALYST
    CHECK(tt_vpn_policy_platform_default() == TT_VPN_POLICY_OS);
#else
    CHECK(tt_vpn_policy_platform_default() == TT_VPN_POLICY_PREFER_PHYSICAL);
#endif
}






static uint32_t ClassV4(const char* ip)
{
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    inet_pton(AF_INET, ip, &sin.sin_addr);
    return tt_peer_address_class((const struct sockaddr*)&sin, sizeof(sin));
}

static uint32_t ClassV6(const char* ip)
{
    struct sockaddr_in6 sin6;
    memset(&sin6, 0, sizeof(sin6));
    sin6.sin6_family = AF_INET6;
    inet_pton(AF_INET6, ip, &sin6.sin6_addr);
    return tt_peer_address_class((const struct sockaddr*)&sin6, sizeof(sin6));
}

static void test_peer_address_class()
{
    
    
    CHECK(ClassV4("34.117.59.81") == TT_PEER_CLASS_GLOBAL);
    CHECK(ClassV4("1.1.1.1")      == TT_PEER_CLASS_GLOBAL);
    CHECK(ClassV4("8.8.8.8")      == TT_PEER_CLASS_GLOBAL);
    CHECK(ClassV4("223.5.5.5")    == TT_PEER_CLASS_GLOBAL);
    CHECK(ClassV6("2606:4700:4700::1111") == TT_PEER_CLASS_GLOBAL);

    
    
    CHECK((ClassV4("198.18.0.0")   & TT_PEER_CLASS_FAKE_IP) != 0);
    CHECK((ClassV4("198.18.0.5")   & TT_PEER_CLASS_FAKE_IP) != 0);
    CHECK((ClassV4("198.19.255.255") & TT_PEER_CLASS_FAKE_IP) != 0);
    CHECK((ClassV4("198.18.0.5")   & TT_PEER_CLASS_PRIVATE) != 0);
    
    CHECK(ClassV4("198.17.255.255") == TT_PEER_CLASS_GLOBAL);
    CHECK(ClassV4("198.20.0.0")     == TT_PEER_CLASS_GLOBAL);
    
    
    CHECK(ClassV4("28.0.0.1")   == TT_PEER_CLASS_FAKE_IP);
    CHECK(ClassV4("28.255.255.255") == TT_PEER_CLASS_FAKE_IP);
    CHECK(ClassV4("27.255.255.255") == TT_PEER_CLASS_GLOBAL);
    CHECK(ClassV4("29.0.0.0")       == TT_PEER_CLASS_GLOBAL);

    
    
    CHECK((ClassV4("10.10.0.1")      & TT_PEER_CLASS_PRIVATE) != 0);
    CHECK((ClassV4("10.0.0.0")       & TT_PEER_CLASS_PRIVATE) != 0);
    CHECK((ClassV4("10.255.255.255") & TT_PEER_CLASS_PRIVATE) != 0);
    CHECK((ClassV4("172.16.0.1")     & TT_PEER_CLASS_PRIVATE) != 0);
    CHECK((ClassV4("172.31.255.255") & TT_PEER_CLASS_PRIVATE) != 0);
    CHECK((ClassV4("192.168.3.1")    & TT_PEER_CLASS_PRIVATE) != 0);
    CHECK((ClassV4("100.64.0.1")     & TT_PEER_CLASS_PRIVATE) != 0);
    CHECK((ClassV4("127.0.0.1")      & TT_PEER_CLASS_PRIVATE) != 0);
    CHECK((ClassV4("169.254.1.1")    & TT_PEER_CLASS_PRIVATE) != 0);
    CHECK((ClassV4("0.0.0.0")        & TT_PEER_CLASS_PRIVATE) != 0);
    CHECK((ClassV4("192.0.2.1")      & TT_PEER_CLASS_PRIVATE) != 0);
    CHECK((ClassV4("198.51.100.1")   & TT_PEER_CLASS_PRIVATE) != 0);
    CHECK((ClassV4("203.0.113.1")    & TT_PEER_CLASS_PRIVATE) != 0);
    CHECK((ClassV4("224.0.0.1")      & TT_PEER_CLASS_PRIVATE) != 0);
    CHECK((ClassV4("255.255.255.255")& TT_PEER_CLASS_PRIVATE) != 0);
    
    CHECK(ClassV4("9.255.255.255")   == TT_PEER_CLASS_GLOBAL);
    CHECK(ClassV4("11.0.0.0")        == TT_PEER_CLASS_GLOBAL);
    CHECK(ClassV4("172.15.255.255")  == TT_PEER_CLASS_GLOBAL);
    CHECK(ClassV4("172.32.0.0")      == TT_PEER_CLASS_GLOBAL);
    CHECK(ClassV4("192.167.255.255") == TT_PEER_CLASS_GLOBAL);
    CHECK(ClassV4("192.169.0.0")     == TT_PEER_CLASS_GLOBAL);
    CHECK(ClassV4("100.63.255.255")  == TT_PEER_CLASS_GLOBAL);
    CHECK(ClassV4("100.128.0.0")     == TT_PEER_CLASS_GLOBAL);

    
    CHECK((ClassV6("fd00::1")   & TT_PEER_CLASS_PRIVATE) != 0);  
    CHECK((ClassV6("fc00::1")   & TT_PEER_CLASS_PRIVATE) != 0);
    CHECK((ClassV6("fe80::1")   & TT_PEER_CLASS_PRIVATE) != 0);  
    CHECK((ClassV6("ff02::1")   & TT_PEER_CLASS_PRIVATE) != 0);  
    CHECK((ClassV6("::1")       & TT_PEER_CLASS_PRIVATE) != 0);  
    CHECK((ClassV6("::")        & TT_PEER_CLASS_PRIVATE) != 0);  
    CHECK((ClassV6("2001:db8::1")& TT_PEER_CLASS_PRIVATE) != 0); 
    CHECK(ClassV6("2001:4860:4860::8888") == TT_PEER_CLASS_GLOBAL);
    
    CHECK((ClassV6("::ffff:10.10.0.1")   & TT_PEER_CLASS_PRIVATE) != 0);
    CHECK((ClassV6("::ffff:198.18.0.5")  & TT_PEER_CLASS_FAKE_IP) != 0);
    CHECK(ClassV6("::ffff:34.117.59.81") == TT_PEER_CLASS_GLOBAL);

    
    CHECK(tt_peer_address_class(NULL, 0) == TT_PEER_CLASS_GLOBAL);
    struct sockaddr_in shortSin;
    memset(&shortSin, 0, sizeof(shortSin));
    shortSin.sin_family = AF_INET;
    inet_pton(AF_INET, "10.10.0.1", &shortSin.sin_addr);
    
    CHECK(tt_peer_address_class((const struct sockaddr*)&shortSin, 4)
          == TT_PEER_CLASS_GLOBAL);
    struct sockaddr_un_like { unsigned short fam; char path[16]; } other;
    memset(&other, 0, sizeof(other));
    other.fam = AF_UNIX;
    CHECK(tt_peer_address_class((const struct sockaddr*)&other, sizeof(other))
          == TT_PEER_CLASS_GLOBAL);
}
















static TTPinVerdict OldVerdict(TTVpnPolicy policy,
                               uint32_t bound,
                               uint32_t global_idx,
                               int isFakeIP)
{
    if (bound == 0) {
        
        return TT_PIN_VERDICT_KEEP;
    }
    if (policy == TT_VPN_POLICY_FORCE_PHYSICAL) {
        return TT_PIN_VERDICT_KEEP;
    }
    const int mismatch = (global_idx != 0 && global_idx != bound);
    const int fakeFallback = (global_idx == 0 && isFakeIP);
    return (mismatch || fakeFallback) ? TT_PIN_VERDICT_UNPIN : TT_PIN_VERDICT_KEEP;
}

static TTPinVerdict New(TTVpnPolicy policy, uint32_t bound,
                        uint32_t scoped, uint32_t global_idx, uint32_t cls)
{
    return tt_vpn_pin_recheck(policy, bound, scoped, global_idx, cls, NULL);
}




#define IF_PHY  14u
#define IF_VPN  22u
#define R_UNK   TT_ROUTE_IFINDEX_UNKNOWN
#define R_NOR   TT_ROUTE_IFINDEX_NO_ROUTE




static void test_recheck_the_bug_being_fixed()
{
    TTPinRecheckReason why = TT_PIN_REASON_SCOPED_MISMATCH;

    
    CHECK(tt_vpn_pin_recheck(TT_VPN_POLICY_PREFER_PHYSICAL, IF_PHY,
                             IF_PHY, IF_VPN,
                             TT_PEER_CLASS_GLOBAL, &why)
          == TT_PIN_VERDICT_KEEP);
    CHECK(why == TT_PIN_REASON_OK);
    
    
    CHECK(OldVerdict(TT_VPN_POLICY_PREFER_PHYSICAL, IF_PHY, IF_VPN, 0)
          == TT_PIN_VERDICT_UNPIN);

    
    CHECK(New(TT_VPN_POLICY_FORCE_PHYSICAL, IF_PHY, IF_PHY, IF_VPN,
              TT_PEER_CLASS_GLOBAL) == TT_PIN_VERDICT_KEEP);

    
    CHECK(tt_vpn_pin_recheck(TT_VPN_POLICY_OS, IF_PHY, IF_PHY, IF_VPN,
                             TT_PEER_CLASS_GLOBAL, &why)
          == TT_PIN_VERDICT_UNPIN);
    CHECK(why == TT_PIN_REASON_GLOBAL_MISMATCH);
}









static void test_recheck_blackhole_protection_not_weakened()
{
    TTPinRecheckReason why = TT_PIN_REASON_OK;
    const uint32_t kFake    = TT_PEER_CLASS_FAKE_IP | TT_PEER_CLASS_PRIVATE;
    const uint32_t kPrivate = TT_PEER_CLASS_PRIVATE;

    
    CHECK(tt_vpn_pin_recheck(TT_VPN_POLICY_PREFER_PHYSICAL, IF_PHY,
                             IF_PHY, IF_VPN,
                             kFake, &why) == TT_PIN_VERDICT_UNPIN);
    CHECK(why == TT_PIN_REASON_BLACKHOLE_RISK);

    
    CHECK(tt_vpn_pin_recheck(TT_VPN_POLICY_PREFER_PHYSICAL, IF_PHY,
                             IF_PHY, R_UNK, kFake, &why)
          == TT_PIN_VERDICT_UNPIN);
    CHECK(why == TT_PIN_REASON_BLACKHOLE_RISK);

    
    
    
    CHECK(tt_vpn_pin_recheck(TT_VPN_POLICY_PREFER_PHYSICAL, IF_PHY,
                             IF_PHY, IF_VPN, kPrivate, &why)
          == TT_PIN_VERDICT_UNPIN);
    CHECK(why == TT_PIN_REASON_BLACKHOLE_RISK);

    
    
    CHECK(New(TT_VPN_POLICY_PREFER_PHYSICAL, IF_PHY, IF_PHY, IF_PHY, kPrivate)
          == TT_PIN_VERDICT_KEEP);

    
    
    const uint32_t kClasses[] = { TT_PEER_CLASS_FAKE_IP, TT_PEER_CLASS_PRIVATE,
                                  TT_PEER_CLASS_FAKE_IP | TT_PEER_CLASS_PRIVATE };
    const uint32_t kIdx[] = { R_UNK, IF_PHY, IF_VPN, 99u, R_NOR };
    for (size_t c = 0; c < sizeof(kClasses)/sizeof(kClasses[0]); ++c) {
        for (size_t g = 0; g < sizeof(kIdx)/sizeof(kIdx[0]); ++g) {
            for (size_t sc = 0; sc < sizeof(kIdx)/sizeof(kIdx[0]); ++sc) {
                const int isFake =
                    (kClasses[c] & TT_PEER_CLASS_FAKE_IP) != 0 ? 1 : 0;
                if (OldVerdict(TT_VPN_POLICY_PREFER_PHYSICAL, IF_PHY,
                               kIdx[g], isFake) == TT_PIN_VERDICT_UNPIN) {
                    CHECK(New(TT_VPN_POLICY_PREFER_PHYSICAL, IF_PHY,
                              kIdx[sc], kIdx[g], kClasses[c])
                          == TT_PIN_VERDICT_UNPIN);
                }
            }
        }
    }
}


static void test_recheck_binding_validity()
{
    TTPinRecheckReason why = TT_PIN_REASON_OK;

    
    CHECK(tt_vpn_pin_recheck(TT_VPN_POLICY_PREFER_PHYSICAL, IF_PHY,
                             IF_VPN, IF_VPN,
                             TT_PEER_CLASS_GLOBAL, &why)
          == TT_PIN_VERDICT_UNPIN);
    CHECK(why == TT_PIN_REASON_SCOPED_MISMATCH);

    
    
    CHECK(tt_vpn_pin_recheck(TT_VPN_POLICY_PREFER_PHYSICAL, IF_PHY,
                             R_NOR, IF_VPN, TT_PEER_CLASS_GLOBAL, &why)
          == TT_PIN_VERDICT_UNPIN);
    CHECK(why == TT_PIN_REASON_SCOPED_NO_ROUTE);

    
    
    CHECK(tt_vpn_pin_recheck(TT_VPN_POLICY_PREFER_PHYSICAL, IF_PHY,
                             R_UNK, R_UNK, TT_PEER_CLASS_GLOBAL, &why)
          == TT_PIN_VERDICT_KEEP);
    CHECK(why == TT_PIN_REASON_OK);
    CHECK(New(TT_VPN_POLICY_PREFER_PHYSICAL, IF_PHY, R_UNK, IF_VPN,
              TT_PEER_CLASS_GLOBAL) == TT_PIN_VERDICT_KEEP);

    
    const uint32_t kIdx[] = { R_UNK, IF_PHY, IF_VPN, R_NOR };
    for (size_t i = 0; i < sizeof(kIdx)/sizeof(kIdx[0]); ++i) {
        for (size_t j = 0; j < sizeof(kIdx)/sizeof(kIdx[0]); ++j) {
            CHECK(tt_vpn_pin_recheck(TT_VPN_POLICY_PREFER_PHYSICAL, 0,
                                     kIdx[i], kIdx[j],
                                     TT_PEER_CLASS_FAKE_IP, &why)
                  == TT_PIN_VERDICT_KEEP);
            CHECK(why == TT_PIN_REASON_OK);
        }
    }
}




static void test_recheck_matches_old_behaviour_where_required()
{
    const uint32_t kIdx[] = { R_UNK, IF_PHY, IF_VPN, 99u, R_NOR };
    const uint32_t kClasses[] = {
        TT_PEER_CLASS_GLOBAL,
        TT_PEER_CLASS_FAKE_IP,
        TT_PEER_CLASS_PRIVATE,
        TT_PEER_CLASS_FAKE_IP | TT_PEER_CLASS_PRIVATE,
    };
    const uint32_t kBound[] = { 0u, IF_PHY, IF_VPN };
    const TTVpnPolicy kUnchanged[] = {
        TT_VPN_POLICY_OS, TT_VPN_POLICY_FORCE_PHYSICAL, TT_VPN_POLICY_UNSET,
    };

    for (size_t b = 0; b < sizeof(kBound)/sizeof(kBound[0]); ++b) {
      for (size_t sc = 0; sc < sizeof(kIdx)/sizeof(kIdx[0]); ++sc) {
        for (size_t g = 0; g < sizeof(kIdx)/sizeof(kIdx[0]); ++g) {
          for (size_t c = 0; c < sizeof(kClasses)/sizeof(kClasses[0]); ++c) {
            const int isFake = (kClasses[c] & TT_PEER_CLASS_FAKE_IP) != 0 ? 1 : 0;

            
            
            
            
            
            
            
            
            const int kIntendedWidening =
                (kBound[b] != 0) &&
                (kIdx[g] == R_UNK) &&
                (kClasses[c] != TT_PEER_CLASS_GLOBAL) &&
                ((kClasses[c] & TT_PEER_CLASS_FAKE_IP) == 0);

            for (size_t p = 0; p < sizeof(kUnchanged)/sizeof(kUnchanged[0]); ++p) {
                const TTPinVerdict now =
                    New(kUnchanged[p], kBound[b], kIdx[sc], kIdx[g], kClasses[c]);
                const TTPinVerdict before =
                    OldVerdict(kUnchanged[p], kBound[b], kIdx[g], isFake);

                if (kIntendedWidening &&
                    kUnchanged[p] != TT_VPN_POLICY_FORCE_PHYSICAL) {
                    
                    
                    CHECK(before == TT_PIN_VERDICT_KEEP);
                    CHECK(now    == TT_PIN_VERDICT_UNPIN);
                } else {
                    CHECK(now == before);
                }
            }

            
            
            
            
            CHECK(New(TT_VPN_POLICY_PREFER_PHYSICAL, kBound[b],
                      kIdx[g], kIdx[g],
                      TT_PEER_CLASS_GLOBAL)
                  == OldVerdict(TT_VPN_POLICY_PREFER_PHYSICAL, kBound[b],
                                kIdx[g], 0));
          }
        }
      }
    }
}




static void test_os_widened_blackhole_fallback()
{
    TTPinRecheckReason why = TT_PIN_REASON_OK;
    const uint32_t kPrivate = TT_PEER_CLASS_PRIVATE;   

    
    CHECK(tt_vpn_pin_recheck(TT_VPN_POLICY_OS, IF_PHY, IF_PHY, R_UNK,
                             kPrivate, &why) == TT_PIN_VERDICT_UNPIN);
    CHECK(why == TT_PIN_REASON_BLACKHOLE_RISK);
    
    CHECK(OldVerdict(TT_VPN_POLICY_OS, IF_PHY, R_UNK, 0)
          == TT_PIN_VERDICT_KEEP);

    
    CHECK(New(TT_VPN_POLICY_OS, IF_PHY, IF_PHY, R_UNK,
              TT_PEER_CLASS_FAKE_IP) == TT_PIN_VERDICT_UNPIN);

    
    CHECK(New(TT_VPN_POLICY_OS, IF_PHY, IF_PHY, R_UNK,
              TT_PEER_CLASS_GLOBAL) == TT_PIN_VERDICT_KEEP);

    
    CHECK(New(TT_VPN_POLICY_FORCE_PHYSICAL, IF_PHY, IF_PHY, R_UNK,
              kPrivate) == TT_PIN_VERDICT_KEEP);
}

static void test_pin_reason_to_string()
{
    CHECK(strcmp(tt_vpn_pin_reason_to_string(TT_PIN_REASON_OK), "ok") == 0);
    CHECK(strcmp(tt_vpn_pin_reason_to_string(TT_PIN_REASON_SCOPED_MISMATCH),
                 "scoped-mismatch") == 0);
    CHECK(strcmp(tt_vpn_pin_reason_to_string(TT_PIN_REASON_SCOPED_NO_ROUTE),
                 "scoped-no-route") == 0);
    CHECK(strcmp(tt_vpn_pin_reason_to_string(TT_PIN_REASON_BLACKHOLE_RISK),
                 "blackhole-risk") == 0);
    CHECK(strcmp(tt_vpn_pin_reason_to_string(TT_PIN_REASON_GLOBAL_MISMATCH),
                 "global-mismatch") == 0);
    
    CHECK(tt_vpn_pin_reason_to_string((TTPinRecheckReason)99) != NULL);
}

int main()
{
    test_from_string();
    test_to_string();
    test_resolve_matrix();
    test_platform_default();
    test_peer_address_class();
    test_recheck_the_bug_being_fixed();
    test_recheck_blackhole_protection_not_weakened();
    test_recheck_binding_validity();
    test_recheck_matches_old_behaviour_where_required();
    test_os_widened_blackhole_fallback();
    test_pin_reason_to_string();
    if (g_failures == 0) {
        printf("VpnPolicy_test: ALL PASS\n");
        return 0;
    }
    printf("VpnPolicy_test: %d FAILURE(S)\n", g_failures);
    return 1;
}
