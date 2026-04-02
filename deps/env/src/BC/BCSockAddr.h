
#ifndef BCSOCKADDR_INCLUDED__
#define BCSOCKADDR_INCLUDED__

#include <BC/Exports.h>
#include <BC/BCNet.h>
#ifdef BC_PLATFORM_HAVESYSUNH
#include <sys/un.h>
#endif // BC_PLATFORM_HAVESYSUNH

///////////////////////////////////////////////////////////////////////////////
// Namespace : BC
///////////////////////////////////////////////////////////////////////////////

namespace BC
{

///////////////////////////////////////////////////////////////////////////////
// Macros & types
///////////////////////////////////////////////////////////////////////////////

typedef struct BCSockAddrS BCSockAddrS;

typedef struct BCSockAddrLinkS
{
	BCSockAddrS	*	prev;
	BCSockAddrS	*	next;
}BCSockAddrLinkS;

typedef struct BCSockAddrListS
{
	BCSockAddrS	*	head;
	BCSockAddrS	*	tail;
}BCSockAddrListS;

typedef struct BCSockAddrS
{
	union {
		struct sockaddr		sa;
		struct sockaddr_in	sin;
		struct sockaddr_in6	sin6;
#ifdef BC_PLATFORM_HAVESYSUNH
		struct sockaddr_un	sunix;
#endif
	}						type;
	unsigned int			length;		/* XXXRTH beginning? */
	BCSockAddrLinkS			link;
}BCSockAddrS;

typedef BCSockAddrListS		bc_sockaddrlist_t;

#define BC_SOCKADDR_CMPADDR	  0x0001	/*%< compare the address
						 *   sin_addr/sin6_addr */
#define BC_SOCKADDR_CMPPORT 	  0x0002	/*%< compare the port
						 *   sin_port/sin6_port */
#define BC_SOCKADDR_CMPSCOPE     0x0004	/*%< compare the scope
						 *   sin6_scope */
#define BC_SOCKADDR_CMPSCOPEZERO 0x0008	/*%< when comparing scopes
						 *   zero scopes always match */

BC_API BOOL
bc_sockaddr_compare(const BCSockAddrS *a, const BCSockAddrS *b,
		     unsigned int flags);
/*%<
 * Compare the elements of the two address ('a' and 'b') as specified
 * by 'flags' and report if they are equal or not.
 *
 * 'flags' is set from BC_SOCKADDR_CMP*.
 */

BC_API BOOL
bc_sockaddr_equal(const BCSockAddrS *a, const BCSockAddrS *b);
/*%<
 * Return BC_TRUE iff the socket addresses 'a' and 'b' are equal.
 */

BC_API BOOL
bc_sockaddr_eqaddr(const BCSockAddrS *a, const BCSockAddrS *b);
/*%<
 * Return BC_TRUE iff the address parts of the socket addresses
 * 'a' and 'b' are equal, ignoring the ports.
 */

BC_API BOOL
bc_sockaddr_eqaddrprefix(const BCSockAddrS *a, const BCSockAddrS *b,
			  unsigned int prefixlen);
/*%<
 * Return BC_TRUE iff the most significant 'prefixlen' bits of the
 * socket addresses 'a' and 'b' are equal, ignoring the ports.
 * If 'b''s scope is zero then 'a''s scope will be ignored.
 */

#if 0
BC_API unsigned int
bc_sockaddr_hash(const BCSockAddrS *sockaddr, BOOL address_only);
/*%<
 * Return a hash value for the socket address 'sockaddr'.  If 'address_only'
 * is TRUE, the hash value will not depend on the port.
 *
 * IPv6 addresses containing mapped IPv4 addresses generate the same hash
 * value as the equivalent IPv4 address.
 */
#endif

BC_API void
bc_sockaddr_any(BCSockAddrS *sockaddr);
/*%<
 * Return the IPv4 wildcard address.
 */

BC_API void
bc_sockaddr_any6(BCSockAddrS *sockaddr);
/*%<
 * Return the IPv6 wildcard address.
 */

BC_API void
bc_sockaddr_anyofpf(BCSockAddrS *sockaddr, int family);
/*%<
 * Set '*sockaddr' to the wildcard address of protocol family
 * 'family'.
 *
 * Requires:
 * \li	'family' is AF_INET or AF_INET6.
 */

BC_API void
bc_sockaddr_fromin(BCSockAddrS *sockaddr, const struct in_addr *ina,
		    in_port_t port);
/*%<
 * Construct an bc_sockaddr_t from an IPv4 address and port.
 */

BC_API void
bc_sockaddr_fromin6(BCSockAddrS *sockaddr, const struct in6_addr *ina6,
		     in_port_t port);
/*%<
 * Construct an bc_sockaddr_t from an IPv6 address and port.
 */

BC_API void
bc_sockaddr_v6fromin(BCSockAddrS *sockaddr, const struct in_addr *ina,
		      in_port_t port);
/*%<
 * Construct an IPv6 bc_sockaddr_t representing a mapped IPv4 address.
 */

BC_API void
bc_sockaddr_fromnetaddr(BCSockAddrS *sockaddr, const BCNetAddrS *na,
			 in_port_t port);
/*%<
 * Construct an bc_sockaddr_t from an bc_netaddr_t and port.
 */

BC_API int
bc_sockaddr_pf(const BCSockAddrS *sockaddr);
/*%<
 * Get the protocol family of 'sockaddr'.
 *
 * Requires:
 *
 *\li	'sockaddr' is a valid sockaddr with an address family of AF_INET
 *	or AF_INET6.
 *
 * Returns:
 *
 *\li	The protocol family of 'sockaddr', e.g. PF_INET or PF_INET6.
 */

BC_API void
bc_sockaddr_setport(BCSockAddrS *sockaddr, in_port_t port);
/*%<
 * Set the port of 'sockaddr' to 'port'.
 */

BC_API in_port_t
bc_sockaddr_getport(const BCSockAddrS *sockaddr);
/*%<
 * Get the port stored in 'sockaddr'.
 */

BC_API BCRESULT
bc_sockaddr_totext(const BCSockAddrS *sockaddr, BCPString *target);
/*%<
 * Append a text representation of 'sockaddr' to the buffer 'target'.
 * The text will include both the IP address (v4 or v6) and the port.
 * The text is null terminated, but the terminating null is not
 * part of the buffer's used region.
 *
 * Returns:
 * \li	BC_R_SUCCESS
 * \li	BC_R_NOSPACE	The text or the null termination did not fit.
 */

BC_API void
bc_sockaddr_format(const BCSockAddrS *sa, char *array, unsigned int size);
/*%<
 * Format a human-readable representation of the socket address '*sa'
 * into the character array 'array', which is of size 'size'.
 * The resulting string is guaranteed to be null-terminated.
 */

BC_API BOOL
bc_sockaddr_ismulticast(const BCSockAddrS *sa);
/*%<
 * Returns #BC_TRUE if the address is a multicast address.
 */

BC_API BOOL
bc_sockaddr_isexperimental(const BCSockAddrS *sa);
/*
 * Returns BC_TRUE if the address is a experimental (CLASS E) address.
 */

BC_API BOOL
bc_sockaddr_islinklocal(const BCSockAddrS *sa);
/*%<
 * Returns BC_TRUE if the address is a link local address.
 */

BC_API BOOL
bc_sockaddr_issitelocal(const BCSockAddrS *sa);
/*%<
 * Returns BC_TRUE if the address is a sitelocal address.
 */

BC_API BOOL
bc_sockaddr_isbroadcast(const BCSockAddrS *sa);
/*%<
 * Returns BC_TRUE if the address is a broadcast address.
 */

BC_API BOOL
bc_sockaddr_isloopback(const BCSockAddrS *sa);
/*%<
 * Returns BC_TRUE if the address is a loopback address.
 */

BC_API BOOL
bc_sockaddr_iswildcard(const BCSockAddrS *sa);
/*%<
 * Returns BC_TRUE if the address is a wildcard address.
 */

BC_API BCRESULT
bc_sockaddr_frompath(BCSockAddrS *sockaddr, const char *path);
/*
 *  Create a UNIX domain sockaddr that refers to path.
 *
 * Returns:
 * \li	BC_R_NOSPACE
 * \li	BC_R_NOTIMPLEMENTED
 * \li	BC_R_SUCCESS
 */

#define BC_SOCKADDR_FORMATSIZE \
	sizeof("xxxx:xxxx:xxxx:xxxx:xxxx:xxxx:XXX.XXX.XXX.XXX%SSSSSSSSSS#YYYYY")
/*%<
 * Minimum size of array to pass to bc_sockaddr_format().
 */

BC_API const void *
bc_sockaddr_getaddr(const BCSockAddrS *sa);
/*%<
 * Returns pointer of address array.
 */

///////////////////////////////////////////////////////////////////////////////
// End of namespace : BC
///////////////////////////////////////////////////////////////////////////////

};

#endif

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
