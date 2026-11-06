






















#ifndef TT_NETWORK_ROUTE_LOOKUP_H
#define TT_NETWORK_ROUTE_LOOKUP_H

#include <stdint.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif
















#define TT_ROUTE_IFINDEX_UNKNOWN   ((uint32_t)0)
#define TT_ROUTE_IFINDEX_NO_ROUTE  ((uint32_t)0xFFFFFFFFu)






















































uint32_t tt_route_lookup_ifindex(const struct sockaddr* dst,
                                 socklen_t dst_len,
                                 uint32_t scope_ifindex);

#ifdef __cplusplus
}
#endif

#endif 




