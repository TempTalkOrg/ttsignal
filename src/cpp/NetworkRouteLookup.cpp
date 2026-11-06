




























#include "NetworkRouteLookup.h"

#if !defined(__APPLE__) &&                                          \
    !(defined(__linux__) && !defined(__ANDROID__) && !defined(OS_ANDROID)) && \
    !defined(_WIN32)

extern "C" uint32_t tt_route_lookup_ifindex(const struct sockaddr* ,
                                            socklen_t ,
                                            uint32_t )
{
    return TT_ROUTE_IFINDEX_UNKNOWN;
}

#endif




