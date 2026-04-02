
#ifndef BCNET_INCLUDED__
#define BCNET_INCLUDED__

#include <BC/Exports.h>
#include <BC/Platform.h>
#ifdef BC_PLATFORM_HAVESYSUNH
#include <sys/un.h>
#endif // BC_PLATFORM_HAVESYSUNH

#ifdef _WIN32

#include <WinSock2.h>
#include <WS2tcpip.h>


// BC_LANG_BEGINDECLS

// #if _MSC_VER < 1300

// ///////////////////////////////////////////////////////////////////////////////
// // file : ipv6
// ///////////////////////////////////////////////////////////////////////////////


// /***
//  *** Types.
//  ***/

// struct in6_addr {
//         union {
// 		uint8_t	_S6_u8[16];
// 		uint16_t	_S6_u16[8];
// 		uint32_t	_S6_u32[4];
//         } _S6_un;
// };
// #define s6_addr		_S6_un._S6_u8
// #define s6_addr8	_S6_un._S6_u8
// #define s6_addr16	_S6_un._S6_u16
// #define s6_addr32	_S6_un._S6_u32

// #ifndef IN6ADDR_ANY_INIT
// #	define IN6ADDR_ANY_INIT 	{{{ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 }}}
// #endif
// #ifndef IN6ADDR_LOOPBACK_INIT
// #	define IN6ADDR_LOOPBACK_INIT 	{{{ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1 }}}
// #endif

// BC_API extern const struct in6_addr in6addr_any;
// BC_API extern const struct in6_addr in6addr_loopback;

// struct sockaddr_in6 {
// #ifdef BC_PLATFORM_HAVESALEN
// 	uint8_t		sin6_len;
// 	uint8_t		sin6_family;
// #else
// 	uint16_t		sin6_family;
// #endif
// 	uint16_t		sin6_port;
// 	uint32_t		sin6_flowinfo;
// 	struct in6_addr		sin6_addr;
// 	uint32_t		sin6_scope_id;
// };

// #ifdef BC_PLATFORM_HAVESALEN
// #define SIN6_LEN 1
// #endif

// /*%
//  * Unspecified
//  */
// #define IN6_IS_ADDR_UNSPECIFIED(a)      \
//         (((a)->s6_addr32[0] == 0) &&    \
//          ((a)->s6_addr32[1] == 0) &&    \
//          ((a)->s6_addr32[2] == 0) &&    \
//          ((a)->s6_addr32[3] == 0))

// /*%
//  * Loopback
//  */
// #define IN6_IS_ADDR_LOOPBACK(a)         \
//         (((a)->s6_addr32[0] == 0) &&    \
//          ((a)->s6_addr32[1] == 0) &&    \
//          ((a)->s6_addr32[2] == 0) &&    \
//          ((a)->s6_addr32[3] == htonl(1)))

// /*%
//  * IPv4 compatible
//  */
// #define IN6_IS_ADDR_V4COMPAT(a)         \
//         (((a)->s6_addr32[0] == 0) &&    \
//          ((a)->s6_addr32[1] == 0) &&    \
//          ((a)->s6_addr32[2] == 0) &&    \
//          ((a)->s6_addr32[3] != 0) &&    \
//          ((a)->s6_addr32[3] != htonl(1)))

// /*%
//  * Mapped
//  */
// #define IN6_IS_ADDR_V4MAPPED(a)               \
//         (((a)->s6_addr32[0] == 0) &&          \
//          ((a)->s6_addr32[1] == 0) &&          \
//          ((a)->s6_addr32[2] == htonl(0x0000ffff)))

// /*%
//  * Multicast
//  */
// #define IN6_IS_ADDR_MULTICAST(a)	\
// 	((a)->s6_addr8[0] == 0xffU)

// /*%
//  * Unicast link / site local.
//  */
// #ifndef IN6_IS_ADDR_LINKLOCAL
// #define IN6_IS_ADDR_LINKLOCAL(a)	\
// 	(((a)->s6_addr[0] == 0xfe) && (((a)->s6_addr[1] & 0xc0) == 0x80))
// #endif
// #ifndef IN6_IS_ADDR_SITELOCAL
// #define IN6_IS_ADDR_SITELOCAL(a)	\
// 	(((a)->s6_addr[0] == 0xfe) && (((a)->s6_addr[1] & 0xc0) == 0xc0))
// #endif

// #endif // _MSC_VER < 1300

// BC_LANG_ENDDECLS

///////////////////////////////////////////////////////////////////////////////
// file : net
///////////////////////////////////////////////////////////////////////////////

#ifndef INADDR_LOOPBACK
#define INADDR_LOOPBACK 0x7f000001UL
#endif

#ifndef BC_PLATFORM_HAVEIN6PKTINFO
struct in6_pktinfo {
	struct in6_addr ipi6_addr;    /* src/dst IPv6 address */
	unsigned int    ipi6_ifindex; /* send/recv interface index */
};
#endif

// #if _MSC_VER < 1300
// #define in6addr_any bc_in6addr_any
// #define in6addr_loopback bc_in6addr_loopback
// #endif // _MSC_VER < 1300

/*
 * Ensure type in_port_t is defined.
 */
#ifdef BC_PLATFORM_NEEDPORTT
typedef uint16_t in_port_t;
#endif

/*
 * If this system does not have MSG_TRUNC (as returned from recvmsg())
 * BC_PLATFORM_RECVOVERFLOW will be defined.  This will enable the MSG_TRUNC
 * faking code in socket.c.
 */
#ifndef MSG_TRUNC
#define BC_PLATFORM_RECVOVERFLOW
#endif

#define BC__IPADDR(x)	((uint32_t)htonl((uint32_t)(x)))
#define BC__IPADDR2(x)	((uint32_t)ntohl((uint32_t)(x)))

// 224.0.0.0/24 to 239.0.0.0/24
#define BC_IPADDR_ISMULTICAST(i) \
		((BC__IPADDR2(i) & 0xF0000000) == 0xE0000000)

#define BC_IPADDR_ISEXPERIMENTAL(i) \
		((BC__IPADDR2(i) & 0xF0000000) == 0xF0000000)

// 169.254.0.0/16
#define BC_IPADDR_ISLINKLOCAL(i) \
		((BC__IPADDR2(i) & 0xFFFF0000) == 0xA9FE0000)

#define BC_IPADDR_ISSITELOCAL(i) \
		(((BC__IPADDR2(i) & 0xFFFF0000) == 0xA9FE0000) || \
		 ((BC__IPADDR2(i) & 0xFFFF0000) == 0xA9FE0000) || \
		 (BC__IPADDR2(i) >= 0xAC100000 && BC__IPADDR2(i) <= 0xAC1FFFFF))

#define BC_IPADDR_ISBROADCAST(i) \
		((uint32_t)(i)  == INADDR_NONE)

#define BC_IPADDR_ISWILDCARD(i) \
		((uint32_t)(i)  == INADDR_ANY)

// 127.0.0.1 to 127.255.255.255
#define BC_IPADDR_ISLOOPBACK(i) \
		((BC__IPADDR2(i) & 0xFF000000) == 0x7F000000)

/*%
 * Wildcard
 */
#define IN6_IS_ADDR_WILDCARD(a)         \
        (((a)->s6_words[0] == 0) &&    \
         ((a)->s6_words[1] == 0) &&    \
         ((a)->s6_words[2] == 0) &&    \
         ((a)->s6_words[3] == 0) &&    \
		 ((a)->s6_words[4] == 0) &&    \
		 ((a)->s6_words[5] == 0) &&    \
		 ((a)->s6_words[6] == 0) &&    \
		 ((a)->s6_words[7] == 0))

/*
 * Fix the FD_SET and FD_CLR Macros to properly cast
 */
#undef FD_CLR
#define FD_CLR(fd, set) do { \
    u_int __i; \
    for (__i = 0; __i < ((fd_set FAR *)(set))->fd_count; __i++) { \
	if (((fd_set FAR *)(set))->fd_array[__i] == (SOCKET) fd) { \
	    while (__i < ((fd_set FAR *)(set))->fd_count-1) { \
		((fd_set FAR *)(set))->fd_array[__i] = \
		    ((fd_set FAR *)(set))->fd_array[__i+1]; \
		__i++; \
	    } \
	    ((fd_set FAR *)(set))->fd_count--; \
	    break; \
	} \
    } \
} while (0)

#undef FD_SET
#define FD_SET(fd, set) do { \
    u_int __i; \
    for (__i = 0; __i < ((fd_set FAR *)(set))->fd_count; __i++) { \
	if (((fd_set FAR *)(set))->fd_array[__i] == (SOCKET)(fd)) { \
	    break; \
	} \
    } \
    if (__i == ((fd_set FAR *)(set))->fd_count) { \
	if (((fd_set FAR *)(set))->fd_count < FD_SETSIZE) { \
	    ((fd_set FAR *)(set))->fd_array[__i] = (SOCKET)(fd); \
	    ((fd_set FAR *)(set))->fd_count++; \
	} \
    } \
} while (0)

#else // !_WIN32


#include <net/if.h>

#include <netinet/in.h>		/* Contractual promise. */
#include <arpa/inet.h>		/* Contractual promise. */
#ifdef BC_PLATFORM_NEEDNETINETIN6H
#include <netinet/in6.h>	/* Required on UnixWare. */
#endif
#ifdef BC_PLATFORM_NEEDNETINET6IN6H
#include <netinet6/in6.h>	/* Required on BSD/OS for in6_pktinfo. */
#endif


#ifndef BC_PLATFORM_HAVEIPV6
///////////////////////////////////////////////////////////////////////////////
// file : ipv6.h
///////////////////////////////////////////////////////////////////////////////

/***
 *** Types.
 ***/

struct in6_addr {
        union {
		uint8_t		_S6_u8[16];
		uint16_t	_S6_u16[8];
		uint32_t	_S6_u32[4];
        } _S6_un;
};
#define s6_addr		_S6_un._S6_u8
#define s6_addr8	_S6_un._S6_u8
#define s6_addr16	_S6_un._S6_u16
#define s6_addr32	_S6_un._S6_u32

#define IN6ADDR_ANY_INIT 	{{{ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 }}}
#define IN6ADDR_LOOPBACK_INIT 	{{{ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1 }}}

BC_API extern const struct in6_addr in6addr_any;
BC_API extern const struct in6_addr in6addr_loopback;

struct sockaddr_in6 {
#ifdef BC_PLATFORM_HAVESALEN
	uint8_t				sin6_len;
	uint8_t				sin6_family;
#else
	uint16_t			sin6_family;
#endif
	uint16_t			sin6_port;
	uint32_t			sin6_flowinfo;
	struct in6_addr		sin6_addr;
	uint32_t			sin6_scope_id;
};

#ifdef BC_PLATFORM_HAVESALEN
#define SIN6_LEN 1
#endif

/*%
 * Unspecified
 */
#define IN6_IS_ADDR_UNSPECIFIED(a)      \
        (((a)->s6_addr32[0] == 0) &&    \
         ((a)->s6_addr32[1] == 0) &&    \
         ((a)->s6_addr32[2] == 0) &&    \
         ((a)->s6_addr32[3] == 0))

/*%
 * Loopback
 */
#define IN6_IS_ADDR_LOOPBACK(a)         \
        (((a)->s6_addr32[0] == 0) &&    \
         ((a)->s6_addr32[1] == 0) &&    \
         ((a)->s6_addr32[2] == 0) &&    \
         ((a)->s6_addr32[3] == htonl(1)))

/*%
 * IPv4 compatible
 */
#define IN6_IS_ADDR_V4COMPAT(a)         \
        (((a)->s6_addr32[0] == 0) &&    \
         ((a)->s6_addr32[1] == 0) &&    \
         ((a)->s6_addr32[2] == 0) &&    \
         ((a)->s6_addr32[3] != 0) &&    \
         ((a)->s6_addr32[3] != htonl(1)))

/*%
 * Mapped
 */
#define IN6_IS_ADDR_V4MAPPED(a)               \
        (((a)->s6_addr32[0] == 0) &&          \
         ((a)->s6_addr32[1] == 0) &&          \
         ((a)->s6_addr32[2] == htonl(0x0000ffff)))

/*%
 * Multicast
 */
#define IN6_IS_ADDR_MULTICAST(a)	\
	((a)->s6_addr8[0] == 0xffU)

/*%
 * Unicast link / site local.
 */
#define IN6_IS_ADDR_LINKLOCAL(a)	\
	(((a)->s6_addr[0] == 0xfe) && (((a)->s6_addr[1] & 0xc0) == 0x80))
#define IN6_IS_ADDR_SITELOCAL(a)	\
	(((a)->s6_addr[0] == 0xfe) && (((a)->s6_addr[1] & 0xc0) == 0xc0))

#endif // BC_PLATFORM_HAVEIPV6

///////////////////////////////////////////////////////////////////////////////
// file : net.h
///////////////////////////////////////////////////////////////////////////////

#ifdef BC_PLATFORM_HAVEINADDR6
#define in6_addr in_addr6	/*%< Required for pre RFC2133 implementations. */
#endif

#ifdef BC_PLATFORM_HAVES6ADDR16
#define s6_addr16   __u6_addr.__u6_addr16
#endif

#ifdef BC_PLATFORM_HAVEIPV6
#ifndef IN6ADDR_ANY_INIT
#ifdef s6_addr
/*%
 * Required for some pre RFC2133 implementations.
 * IN6ADDR_ANY_INIT and IN6ADDR_LOOPBACK_INIT were added in
 * draft-ietf-ipngwg-bsd-api-04.txt or draft-ietf-ipngwg-bsd-api-05.txt.
 * If 's6_addr' is defined then assume that there is a union and three
 * levels otherwise assume two levels required.
 */
#define IN6ADDR_ANY_INIT { { { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 } } }
#else
#define IN6ADDR_ANY_INIT { { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 } }
#endif
#endif

#ifndef IN6ADDR_LOOPBACK_INIT
#ifdef s6_addr
/*% IPv6 address loopback init */
#define IN6ADDR_LOOPBACK_INIT { { { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1 } } }
#else
#define IN6ADDR_LOOPBACK_INIT { { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1 } }
#endif
#endif

#ifndef IN6_IS_ADDR_V4MAPPED
/*% Is IPv6 address V4 mapped? */
#define IN6_IS_ADDR_V4MAPPED(x) \
	 (memcmp((x)->s6_addr, in6addr_any.s6_addr, 10) == 0 && \
	  (x)->s6_addr[10] == 0xff && (x)->s6_addr[11] == 0xff)
#endif

#ifndef IN6_IS_ADDR_V4COMPAT
/*% Is IPv6 address V4 compatible? */
#define IN6_IS_ADDR_V4COMPAT(x) \
	 (memcmp((x)->s6_addr, in6addr_any.s6_addr, 12) == 0 && \
	 ((x)->s6_addr[12] != 0 || (x)->s6_addr[13] != 0 || \
	  (x)->s6_addr[14] != 0 || \
	  ((x)->s6_addr[15] != 0 && (x)->s6_addr[15] != 1)))
#endif

#ifndef IN6_IS_ADDR_MULTICAST
/*% Is IPv6 address multicast? */
#define IN6_IS_ADDR_MULTICAST(a)        ((a)->s6_addr[0] == 0xff)
#endif

#ifndef IN6_IS_ADDR_LINKLOCAL
/*% Is IPv6 address linklocal? */
#define IN6_IS_ADDR_LINKLOCAL(a) \
	(((a)->s6_addr[0] == 0xfe) && (((a)->s6_addr[1] & 0xc0) == 0x80))
#endif

#ifndef IN6_IS_ADDR_SITELOCAL
/*% is IPv6 address sitelocal? */
#define IN6_IS_ADDR_SITELOCAL(a) \
	(((a)->s6_addr[0] == 0xfe) && (((a)->s6_addr[1] & 0xc0) == 0xc0))
#endif


#ifndef IN6_IS_ADDR_LOOPBACK
/*% is IPv6 address loopback? */
#define IN6_IS_ADDR_LOOPBACK(x) \
	(memcmp((x)->s6_addr, in6addr_loopback.s6_addr, 16) == 0)
#endif
#endif

#ifndef AF_INET6
/*% IPv6 */
#define AF_INET6 99
#endif

#ifndef PF_INET6
/*% IPv6 */
#define PF_INET6 AF_INET6
#endif

#ifndef INADDR_LOOPBACK
/*% inaddr loopback */
#define INADDR_LOOPBACK 0x7f000001UL
#endif

#ifndef BC_PLATFORM_HAVEIN6PKTINFO
/*% IPv6 packet info */
struct in6_pktinfo {
	struct in6_addr ipi6_addr;    /*%< src/dst IPv6 address */
	unsigned int    ipi6_ifindex; /*%< send/recv interface index */
};
#endif

#if defined(BC_PLATFORM_HAVEIPV6) && defined(BC_PLATFORM_NEEDIN6ADDRANY)
extern const struct in6_addr bc_net_in6addrany;
/*%
 * Cope with a missing in6addr_any and in6addr_loopback.
 */
#define in6addr_any bc_net_in6addrany
#endif

#if defined(BC_PLATFORM_HAVEIPV6) && defined(BC_PLATFORM_NEEDIN6ADDRLOOPBACK)
extern const struct in6_addr bc_net_in6addrloop;
#define in6addr_loopback bc_net_in6addrloop
#endif

#ifdef BC_PLATFORM_FIXIN6ISADDR
#undef  IN6_IS_ADDR_GEOGRAPHIC
/*!
 * \brief
 * Fix UnixWare 7.1.1's broken IN6_IS_ADDR_* definitions.
 */
#define IN6_IS_ADDR_GEOGRAPHIC(a) (((a)->S6_un.S6_l[0] & 0xE0) == 0x80)
#undef  IN6_IS_ADDR_IPX
#define IN6_IS_ADDR_IPX(a)        (((a)->S6_un.S6_l[0] & 0xFE) == 0x04)
#undef  IN6_IS_ADDR_LINKLOCAL
#define IN6_IS_ADDR_LINKLOCAL(a)  (((a)->S6_un.S6_l[0] & 0xC0FF) == 0x80FE)
#undef  IN6_IS_ADDR_MULTICAST
#define IN6_IS_ADDR_MULTICAST(a)  (((a)->S6_un.S6_l[0] & 0xFF) == 0xFF)
#undef  IN6_IS_ADDR_NSAP
#define IN6_IS_ADDR_NSAP(a)       (((a)->S6_un.S6_l[0] & 0xFE) == 0x02)
#undef  IN6_IS_ADDR_PROVIDER
#define IN6_IS_ADDR_PROVIDER(a)   (((a)->S6_un.S6_l[0] & 0xE0) == 0x40)
#undef  IN6_IS_ADDR_SITELOCAL
#define IN6_IS_ADDR_SITELOCAL(a)  (((a)->S6_un.S6_l[0] & 0xC0FF) == 0xC0FE)
#endif /* BC_PLATFORM_FIXIN6ISADDR */

#ifdef BC_PLATFORM_NEEDPORTT
/*%
 * Ensure type in_port_t is defined.
 */
typedef uint16_t in_port_t;
#endif

#ifndef MSG_TRUNC
/*%
 * If this system does not have MSG_TRUNC (as returned from recvmsg())
 * BC_PLATFORM_RECVOVERFLOW will be defined.  This will enable the MSG_TRUNC
 * faking code in socket.c.
 */
#define BC_PLATFORM_RECVOVERFLOW
#endif

/*% IP address. */
#define BC__IPADDR(x)	((uint32_t)htonl((uint32_t)(x)))
#define BC__IPADDR2(x)	((uint32_t)ntohl((uint32_t)(x)))

/*% Is IP address multicast? */
#define BC_IPADDR_ISMULTICAST(i) \
		(((uint32_t)(i) & BC__IPADDR(0xf0000000)) \
		 == BC__IPADDR(0xe0000000))

#define BC_IPADDR_ISEXPERIMENTAL(i) \
		(((uint32_t)(i) & BC__IPADDR(0xf0000000)) \
		 == BC__IPADDR(0xf0000000))

// 169.254.0.0/16
#define BC_IPADDR_ISLINKLOCAL(i) \
		((BC__IPADDR2(i) & 0xFFFF0000) == 0xA9FE0000)

#define BC_IPADDR_ISSITELOCAL(i) \
		(((BC__IPADDR2(i) & 0xFFFF0000) == 0xA9FE0000) || \
		 ((BC__IPADDR2(i) & 0xFFFF0000) == 0xA9FE0000) || \
		 (BC__IPADDR2(i) >= 0xAC100000 && BC__IPADDR2(i) <= 0xAC1FFFFF))

#define BC_IPADDR_ISBROADCAST(i) \
		((uint32_t)(i)  == INADDR_NONE)

#define BC_IPADDR_ISWILDCARD(i) \
		((uint32_t)(i)  == INADDR_ANY)

// 127.0.0.1 to 127.255.255.255
#define BC_IPADDR_ISLOOPBACK(i) \
		((BC__IPADDR2(i) & 0xFF000000) == 0x7F000000)

/*%
 * Wildcard
 */
#define IN6_IS_ADDR_WILDCARD(a)         \
        (((a)->s6_addr16[0] == 0) &&    \
         ((a)->s6_addr16[1] == 0) &&    \
         ((a)->s6_addr16[2] == 0) &&    \
         ((a)->s6_addr16[3] == 0) &&    \
		 ((a)->s6_addr16[4] == 0) &&    \
		 ((a)->s6_addr16[5] == 0) &&    \
		 ((a)->s6_addr16[6] == 0) &&    \
		 ((a)->s6_addr16[7] == 0))


#endif // _WIN32

///////////////////////////////////////////////////////////////////////////////
// Namespace : BC
///////////////////////////////////////////////////////////////////////////////

namespace BC
{

struct BCSockAddrS;
class BCPString;

/***
 *** Functions.
 ***/

BC_API BCRESULT
bc_net_probeipv4(void);
/*
 * Check if the system's kernel supports IPv4.
 *
 * Returns:
 *
 *	BC_R_SUCCESS		IPv4 is supported.
 *	BC_R_NOTFOUND		IPv4 is not supported.
 *	BC_R_DISABLED		IPv4 is disabled.
 *	BC_R_UNEXPECTED
 */

BC_API BCRESULT
bc_net_probeipv6(void);
/*
 * Check if the system's kernel supports IPv6.
 *
 * Returns:
 *
 *	BC_R_SUCCESS		IPv6 is supported.
 *	BC_R_NOTFOUND		IPv6 is not supported.
 *	BC_R_DISABLED		IPv6 is disabled.
 *	BC_R_UNEXPECTED
 */

BC_API BCRESULT
bc_net_probeunix(void);
/*
 * Check if UNIX domain sockets are supported.
 *
 * Returns:
 *
 *	BC_R_SUCCESS
 *	BC_R_NOTFOUND
 */

BC_API BCRESULT
bc_net_probe_ipv6only(void);
/*
 * Check if the system's kernel supports the IPV6_V6ONLY socket option.
 *
 * Returns:
 *
 *	BC_R_SUCCESS		the option is supported for both TCP and UDP.
 *	BC_R_NOTFOUND		IPv6 itself or the option is not supported.
 *	BC_R_UNEXPECTED
 */

BC_API BCRESULT
bc_net_probe_ipv6pktinfo(void);
/*
 * Check if the system's kernel supports the IPV6_(RECV)PKTINFO socket option
 * for UDP sockets.
 *
 * Returns:
 *
 *	BC_R_SUCCESS		the option is supported.
 *	BC_R_NOTFOUND		IPv6 itself or the option is not supported.
 *	BC_R_UNEXPECTED
 */

BC_API void
bc_net_disableipv4(void);

BC_API void
bc_net_disableipv6(void);

BC_API void
bc_net_enableipv4(void);

BC_API void
bc_net_enableipv6(void);

BC_API BCRESULT
bc_net_getudpportrange(int af, in_port_t *low, in_port_t *high);
/*%<
 * Returns system's default range of ephemeral UDP ports, if defined.
 * If the range is not available or unknown, BC_NET_PORTRANGELOW and
 * BC_NET_PORTRANGEHIGH will be returned.
 *
 * Requires:
 *
 *\li	'low' and 'high' must be non NULL.
 *
 * Returns:
 *
 *\li	*low and *high will be the ports specifying the low and high ends of
 *	the range.
 */

#ifdef BC_PLATFORM_NEEDNTOP
BC_API const char *
bc_net_ntop(int af, const void *src, char *dst, size_t size);
#define inet_ntop bc_net_ntop
#endif

#ifdef BC_PLATFORM_NEEDPTON
BC_API int
bc_net_pton(int af, const char *src, void *dst);
#define inet_pton bc_net_pton
#endif

BC_API int
bc_net_aton(const char *cp, struct in_addr *addr);
#define inet_aton bc_net_aton

///////////////////////////////////////////////////////////////////////////////
// netaddr
///////////////////////////////////////////////////////////////////////////////

typedef struct BCNetAddrS
{
	unsigned int family;
	union {
		struct in_addr in;
		struct in6_addr in6;
#ifdef BC_PLATFORM_HAVESYSUNH
		char un[sizeof(((struct sockaddr_un *)0)->sun_path)];
#endif
	} type;
	uint32_t zone;
}BCNetAddrS;

BC_API BOOL
bc_netaddr_equal(const BCNetAddrS *a, const BCNetAddrS *b);

/*%<
 * Compare network addresses 'a' and 'b'.  Return #BC_TRUE if
 * they are equal, #BC_FALSE if not.
 */

BC_API BOOL
bc_netaddr_eqprefix(const BCNetAddrS *a, const BCNetAddrS *b,
		     unsigned int prefixlen);
/*%<
 * Compare the 'prefixlen' most significant bits of the network
 * addresses 'a' and 'b'.  If 'b''s scope is zero then 'a''s scope is
 * ignored.  Return #BC_TRUE if they are equal, #BC_FALSE if not.
 */

BC_API BCRESULT
bc_netaddr_masktoprefixlen(const BCNetAddrS *s, unsigned int *lenp);
/*%<
 * Convert a netmask in 's' into a prefix length in '*lenp'.
 * The mask should consist of zero or more '1' bits in the most
 * most significant part of the address, followed by '0' bits.
 * If this is not the case, #BC_R_MASKNONCONTIG is returned.
 *
 * Returns:
 *\li	#BC_R_SUCCESS
 *\li	#BC_R_MASKNONCONTIG
 */

BC_API BCRESULT
bc_netaddr_totext(const BCNetAddrS *netaddr, BCPString *target);
/*%<
 * Append a text representation of 'sockaddr' to the buffer 'target'.
 * The text is NOT null terminated.  Handles IPv4 and IPv6 addresses.
 *
 * Returns:
 *\li	#BC_R_SUCCESS
 *\li	#BC_R_NOSPACE	The text or the null termination did not fit.
 *\li	#BC_R_FAILURE	Unspecified failure
 */

BC_API void
bc_netaddr_format(const BCNetAddrS *na, char *array, unsigned int size);
/*%<
 * Format a human-readable representation of the network address '*na'
 * into the character array 'array', which is of size 'size'.
 * The resulting string is guaranteed to be null-terminated.
 */

#define BC_NETADDR_FORMATSIZE \
	sizeof("xxxx:xxxx:xxxx:xxxx:xxxx:xxxx:XXX.XXX.XXX.XXX%SSSSSSSSSS")
/*%<
 * Minimum size of array to pass to bc_netaddr_format().
 */

BC_API void
bc_netaddr_fromsockaddr(BCNetAddrS *netaddr, const BCSockAddrS *source);

BC_API void
bc_netaddr_fromin(BCNetAddrS *netaddr, const struct in_addr *ina);

BC_API void
bc_netaddr_fromin6(BCNetAddrS *netaddr, const struct in6_addr *ina6);

BCRESULT
bc_netaddr_frompath(BCNetAddrS *netaddr, const char *path);

BC_API void
bc_netaddr_setzone(BCNetAddrS *netaddr, uint32_t zone);

BC_API uint32_t
bc_netaddr_getzone(const BCNetAddrS *netaddr);

BC_API void
bc_netaddr_any(BCNetAddrS *netaddr);
/*%<
 * Return the IPv4 wildcard address.
 */

BC_API void
bc_netaddr_any6(BCNetAddrS *netaddr);
/*%<
 * Return the IPv6 wildcard address.
 */

BC_API BOOL
bc_netaddr_ismulticast(BCNetAddrS *na);
/*%<
 * Returns BC_TRUE if the address is a multicast address.
 */

BC_API BOOL
bc_netaddr_isexperimental(BCNetAddrS *na);
/*%<
 * Returns BC_TRUE if the address is a experimental (CLASS E) address.
 */

BC_API BOOL
bc_netaddr_islinklocal(BCNetAddrS *na);
/*%<
 * Returns #BC_TRUE if the address is a link local address.
 */

BC_API BOOL
bc_netaddr_issitelocal(BCNetAddrS *na);
/*%<
 * Returns #BC_TRUE if the address is a site local address.
 */

BC_API BOOL
bc_netaddr_isbroadcast(BCNetAddrS *na);
/*%<
 * Returns BC_TRUE if the address is a broadcast address.
 */

BC_API BOOL
bc_netaddr_isloopback(BCNetAddrS *na);
/*%<
 * Returns #BC_TRUE if the address is a loopback address.
 */

BC_API BOOL
bc_netaddr_iswildcard(BCNetAddrS *na);
/*%<
 * Returns #BC_TRUE if the address is a wildcard address.
 */

BC_API void
bc_netaddr_fromv4mapped(BCNetAddrS *t, const BCNetAddrS *s);
/*%<
 * Convert an IPv6 v4mapped address into an IPv4 address.
 */

BC_API BCRESULT
bc_netaddr_prefixok(const BCNetAddrS *na, unsigned int prefixlen);
/*
 * Test whether the netaddr 'na' and 'prefixlen' are consistant.
 * e.g. prefixlen within range.
 *      na does not have bits set which are not covered by the prefixlen.
 *
 * Returns:
 *	BC_R_SUCCESS
 *	BC_R_RANGE		prefixlen out of range
 *	BC_R_NOTIMPLEMENTED	unsupported family
 *	BC_R_FAILURE		extra bits.
 */

///////////////////////////////////////////////////////////////////////////////
// End of namespace : BC
///////////////////////////////////////////////////////////////////////////////

};

#endif

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
