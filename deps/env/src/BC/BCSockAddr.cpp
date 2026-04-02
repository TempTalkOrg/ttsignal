
#include <BC/Utils.h>
#include <BC/BCNet.h>
#include <BC/BCLog.h>
#include <BC/BCSockAddr.h>


///////////////////////////////////////////////////////////////////////////////
// Namespace : BC
///////////////////////////////////////////////////////////////////////////////

namespace BC
{

///////////////////////////////////////////////////////////////////////////////
// class :
///////////////////////////////////////////////////////////////////////////////


BOOL
bc_sockaddr_equal(const BCSockAddrS *a, const BCSockAddrS *b)
{
	return (bc_sockaddr_compare(a, b, BC_SOCKADDR_CMPADDR|
	                             BC_SOCKADDR_CMPPORT|
	                             BC_SOCKADDR_CMPSCOPE));
}

BOOL
bc_sockaddr_eqaddr(const BCSockAddrS *a, const BCSockAddrS *b)
{
	return (bc_sockaddr_compare(a, b, BC_SOCKADDR_CMPADDR|
	                             BC_SOCKADDR_CMPSCOPE));
}

BOOL
bc_sockaddr_compare(const BCSockAddrS *a, const BCSockAddrS *b,
                     unsigned int flags)
{
	ASSERT(a != NULL && b != NULL);

	if (a->length != b->length)
		return (FALSE);

	/*
	 * We don't just memcmp because the sin_zero field isn't always
	 * zero.
	 */

	if (a->type.sa.sa_family != b->type.sa.sa_family)
		return (FALSE);
	switch (a->type.sa.sa_family)
	{
	case AF_INET:
		if ((flags & BC_SOCKADDR_CMPADDR) != 0 &&
		        memcmp(&a->type.sin.sin_addr, &b->type.sin.sin_addr,
		               sizeof(a->type.sin.sin_addr)) != 0)
			return (FALSE);
		if ((flags & BC_SOCKADDR_CMPPORT) != 0 &&
		        a->type.sin.sin_port != b->type.sin.sin_port)
			return (FALSE);
		break;
	case AF_INET6:
		if ((flags & BC_SOCKADDR_CMPADDR) != 0 &&
		        memcmp(&a->type.sin6.sin6_addr, &b->type.sin6.sin6_addr,
		               sizeof(a->type.sin6.sin6_addr)) != 0)
			return (FALSE);
#ifdef BC_PLATFORM_HAVESCOPEID
		/*
		 * If BC_SOCKADDR_CMPSCOPEZERO is set then don't return
		 * FALSE if one of the scopes in zero.
		 */
		if ((flags & BC_SOCKADDR_CMPSCOPE) != 0 &&
		        a->type.sin6.sin6_scope_id != b->type.sin6.sin6_scope_id &&
		        ((flags & BC_SOCKADDR_CMPSCOPEZERO) == 0 ||
		         (a->type.sin6.sin6_scope_id != 0 &&
		          b->type.sin6.sin6_scope_id != 0)))
			return (FALSE);
#endif
		if ((flags & BC_SOCKADDR_CMPPORT) != 0 &&
		        a->type.sin6.sin6_port != b->type.sin6.sin6_port)
			return (FALSE);
		break;
	default:
		if (memcmp(&a->type, &b->type, a->length) != 0)
			return (FALSE);
	}
	return (TRUE);
}

BOOL
bc_sockaddr_eqaddrprefix(const BCSockAddrS *a, const BCSockAddrS *b,
                          unsigned int prefixlen)
{
	BCNetAddrS na, nb;
	bc_netaddr_fromsockaddr(&na, a);
	bc_netaddr_fromsockaddr(&nb, b);
	return (bc_netaddr_eqprefix(&na, &nb, prefixlen));
}

BCRESULT
bc_sockaddr_totext(const BCSockAddrS *sockaddr, BCPString *target)
{
	BCRESULT result;
	BCNetAddrS netaddr;
	char pbuf[sizeof("65000")];
	unsigned int plen;

	ASSERT(sockaddr != NULL);

	/*
	 * Do the port first, giving us the opportunity to check for
	 * unsupported address families before calling
	 * bc_netaddr_fromsockaddr().
	 */
	switch (sockaddr->type.sa.sa_family)
	{
	case AF_INET:
		snprintf(pbuf, sizeof(pbuf), "%u", ntohs(sockaddr->type.sin.sin_port));
		break;
	case AF_INET6:
		snprintf(pbuf, sizeof(pbuf), "%u", ntohs(sockaddr->type.sin6.sin6_port));
		break;
#ifdef BC_PLATFORM_HAVESYSUNH
	case AF_UNIX:
		plen = strlen(sockaddr->type.sunix.sun_path);
		target->append((const char*)sockaddr->type.sunix.sun_path, plen);

		/*
		 * Null terminate after used region.
		 */
		target->append((char)0);

		return (BC_R_SUCCESS);
#endif
	default:
		return (BC_R_FAILURE);
	}

	plen = strlen(pbuf);
	ASSERT(plen < sizeof(pbuf));

	bc_netaddr_fromsockaddr(&netaddr, sockaddr);
	result = bc_netaddr_totext(&netaddr, target);
	if (result != BC_R_SUCCESS)
		return (result);

	target->append((const char *)":", 1);
	target->append((const char *)pbuf, plen);

	/*
	 * Null terminate after used region.
	 */
	target->append((char)0);

	return (BC_R_SUCCESS);
}

void
bc_sockaddr_format(const BCSockAddrS *sa, char *array, unsigned int size)
{
	BCRESULT result;
	BCPString buf;

	result = bc_sockaddr_totext(sa, &buf);
	if (result != BC_R_SUCCESS)
	{
		/*
		 * The message is the same as in netaddr.c.
		 */
		snprintf(array, size, "<unknown address, family %u>",
		         sa->type.sa.sa_family);
		array[size - 1] = '\0';
	}
	memcpy2(array, buf.c_str(), BCMIN(size, buf.length()));
}

#if 0
unsigned int
bc_sockaddr_hash(const BCSockAddrS *sockaddr, BOOL address_only)
{
	unsigned int length = 0;
	const unsigned char *s = NULL;
	unsigned int h = 0;
	unsigned int g;
	unsigned int p = 0;
	const struct in6_addr *in6;

	ASSERT(sockaddr != NULL);

	switch (sockaddr->type.sa.sa_family)
	{
	case AF_INET:
		s = (const unsigned char *)&sockaddr->type.sin.sin_addr;
		p = ntohs(sockaddr->type.sin.sin_port);
		length = sizeof(sockaddr->type.sin.sin_addr.s_addr);
		break;
	case AF_INET6:
		in6 = &sockaddr->type.sin6.sin6_addr;
		if (IN6_IS_ADDR_V4MAPPED(in6))
		{
			s = (const unsigned char *)&in6[12];
			length = sizeof(sockaddr->type.sin.sin_addr.s_addr);
		}
		else
		{
			s = (const unsigned char *)in6;
			length = sizeof(sockaddr->type.sin6.sin6_addr);
		}
		p = ntohs(sockaddr->type.sin6.sin6_port);
		break;
	default:
		LogError(_LOCAL_, "unknown address family: %d",
		                 (int)sockaddr->type.sa.sa_family);
		s = (const unsigned char *)&sockaddr->type;
		length = sockaddr->length;
		p = 0;
	}

	h = bc_hash_calc(s, length, TRUE);
	if (!address_only)
	{
		g = bc_hash_calc((const unsigned char *)&p, sizeof(p),
		                  TRUE);
		h = h ^ g; /* XXX: we should concatenate h and p first */
	}

	return (h);
}
#endif

void
bc_sockaddr_any(BCSockAddrS *sockaddr)
{
	memset(sockaddr, 0, sizeof(*sockaddr));
	sockaddr->type.sin.sin_family = AF_INET;
#ifdef BC_PLATFORM_HAVESALEN
	sockaddr->type.sin.sin_len = sizeof(sockaddr->type.sin);
#endif
	sockaddr->type.sin.sin_addr.s_addr = INADDR_ANY;
	sockaddr->type.sin.sin_port = 0;
	sockaddr->length = sizeof(sockaddr->type.sin);
	memset(&sockaddr->link, 0, sizeof(sockaddr->link));
}

void
bc_sockaddr_any6(BCSockAddrS *sockaddr)
{
	memset(sockaddr, 0, sizeof(*sockaddr));
	sockaddr->type.sin6.sin6_family = AF_INET6;
#ifdef BC_PLATFORM_HAVESALEN
	sockaddr->type.sin6.sin6_len = sizeof(sockaddr->type.sin6);
#endif
	sockaddr->type.sin6.sin6_addr = in6addr_any;
	sockaddr->type.sin6.sin6_port = 0;
	sockaddr->length = sizeof(sockaddr->type.sin6);
	memset(&sockaddr->link, 0, sizeof(sockaddr->link));
}

void
bc_sockaddr_fromin(BCSockAddrS *sockaddr, const struct in_addr *ina,
                    in_port_t port)
{
	memset(sockaddr, 0, sizeof(*sockaddr));
	sockaddr->type.sin.sin_family = AF_INET;
#ifdef BC_PLATFORM_HAVESALEN
	sockaddr->type.sin.sin_len = sizeof(sockaddr->type.sin);
#endif
	sockaddr->type.sin.sin_addr = *ina;
	sockaddr->type.sin.sin_port = htons(port);
	sockaddr->length = sizeof(sockaddr->type.sin);
	memset(&sockaddr->link, 0, sizeof(sockaddr->link));
}

void
bc_sockaddr_anyofpf(BCSockAddrS *sockaddr, int pf)
{
	switch (pf)
	{
	case AF_INET:
		bc_sockaddr_any(sockaddr);
		break;
	case AF_INET6:
		bc_sockaddr_any6(sockaddr);
		break;
	default:
		ASSERT(0);
	}
}

void
bc_sockaddr_fromin6(BCSockAddrS *sockaddr, const struct in6_addr *ina6,
                     in_port_t port)
{
	memset(sockaddr, 0, sizeof(*sockaddr));
	sockaddr->type.sin6.sin6_family = AF_INET6;
#ifdef BC_PLATFORM_HAVESALEN
	sockaddr->type.sin6.sin6_len = sizeof(sockaddr->type.sin6);
#endif
	sockaddr->type.sin6.sin6_addr = *ina6;
	sockaddr->type.sin6.sin6_port = htons(port);
	sockaddr->length = sizeof(sockaddr->type.sin6);
	memset(&sockaddr->link, 0, sizeof(sockaddr->link));
}

void
bc_sockaddr_v6fromin(BCSockAddrS *sockaddr, const struct in_addr *ina,
                      in_port_t port)
{
	memset(sockaddr, 0, sizeof(*sockaddr));
	sockaddr->type.sin6.sin6_family = AF_INET6;
#ifdef BC_PLATFORM_HAVESALEN
	sockaddr->type.sin6.sin6_len = sizeof(sockaddr->type.sin6);
#endif
	sockaddr->type.sin6.sin6_addr.s6_addr[10] = 0xff;
	sockaddr->type.sin6.sin6_addr.s6_addr[11] = 0xff;
	memcpy(&sockaddr->type.sin6.sin6_addr.s6_addr[12], ina, 4);
	sockaddr->type.sin6.sin6_port = htons(port);
	sockaddr->length = sizeof(sockaddr->type.sin6);
	memset(&sockaddr->link, 0, sizeof(sockaddr->link));;
}

int
bc_sockaddr_pf(const BCSockAddrS *sockaddr)
{

	/*
	 * Get the protocol family of 'sockaddr'.
	 */

#if (AF_INET == PF_INET && AF_INET6 == PF_INET6 && AF_UNIX == PF_UNIX)
	/*
	 * Assume that PF_xxx == AF_xxx for all AF and PF.
	 */
	return (sockaddr->type.sa.sa_family);
#else
	switch (sockaddr->type.sa.sa_family)
	{
	case AF_UNIX:
		return (PF_UNIX);
	case AF_INET:
		return (PF_INET);
	case AF_INET6:
		return (PF_INET6);
	default:
		LogFatal(_LOCAL_, "unknown address family: %d",
		            (int)sockaddr->type.sa.sa_family);
	}
#endif
}

void
bc_sockaddr_fromnetaddr(BCSockAddrS *sockaddr, const BCNetAddrS *na,
                         in_port_t port)
{
	memset(sockaddr, 0, sizeof(*sockaddr));
	sockaddr->type.sin.sin_family = na->family;
	switch (na->family)
	{
	case AF_INET:
		sockaddr->length = sizeof(sockaddr->type.sin);
#ifdef BC_PLATFORM_HAVESALEN
		sockaddr->type.sin.sin_len = sizeof(sockaddr->type.sin);
#endif
		sockaddr->type.sin.sin_addr = na->type.in;
		sockaddr->type.sin.sin_port = htons(port);
		break;
	case AF_INET6:
		sockaddr->length = sizeof(sockaddr->type.sin6);
#ifdef BC_PLATFORM_HAVESALEN
		sockaddr->type.sin6.sin6_len = sizeof(sockaddr->type.sin6);
#endif
		memcpy(&sockaddr->type.sin6.sin6_addr, &na->type.in6, 16);
#ifdef BC_PLATFORM_HAVESCOPEID
		sockaddr->type.sin6.sin6_scope_id = bc_netaddr_getzone(na);
#endif
		sockaddr->type.sin6.sin6_port = htons(port);
		break;
	default:
		ASSERT(0);
	}
	memset(&sockaddr->link, 0, sizeof(sockaddr->link));
}

void
bc_sockaddr_setport(BCSockAddrS *sockaddr, in_port_t port)
{
	switch (sockaddr->type.sa.sa_family)
	{
	case AF_INET:
		sockaddr->type.sin.sin_port = htons(port);
		break;
	case AF_INET6:
		sockaddr->type.sin6.sin6_port = htons(port);
		break;
	default:
		LogFatal(_LOCAL_, "unknown address family: %d",
		            (int)sockaddr->type.sa.sa_family);
	}
}

in_port_t
bc_sockaddr_getport(const BCSockAddrS *sockaddr)
{
	in_port_t port = 0;

	switch (sockaddr->type.sa.sa_family)
	{
	case AF_UNIX:
		port = 0;
		break;
	case AF_INET:
		port = ntohs(sockaddr->type.sin.sin_port);
		break;
	case AF_INET6:
		port = ntohs(sockaddr->type.sin6.sin6_port);
		break;
	default:
		LogFatal(_LOCAL_, "unknown address family: %d",
		            (int)sockaddr->type.sa.sa_family);
	}

	return (port);
}

BOOL
bc_sockaddr_ismulticast(const BCSockAddrS *sockaddr)
{
	BCNetAddrS netaddr;

	if (sockaddr->type.sa.sa_family == AF_INET ||
	    sockaddr->type.sa.sa_family == AF_INET6)
	{
		bc_netaddr_fromsockaddr(&netaddr, sockaddr);
		return (bc_netaddr_ismulticast(&netaddr));
	}
	return (FALSE);
}

BOOL
bc_sockaddr_isexperimental(const BCSockAddrS *sockaddr)
{
	BCNetAddrS netaddr;

	if (sockaddr->type.sa.sa_family == AF_INET)
	{
		bc_netaddr_fromsockaddr(&netaddr, sockaddr);
		return (bc_netaddr_isexperimental(&netaddr));
	}
	return (FALSE);
}

BOOL
bc_sockaddr_issitelocal(const BCSockAddrS *sockaddr)
{
	BCNetAddrS netaddr;

	if (sockaddr->type.sa.sa_family == AF_INET ||
		sockaddr->type.sa.sa_family == AF_INET6)
	{
		bc_netaddr_fromsockaddr(&netaddr, sockaddr);
		return (bc_netaddr_issitelocal(&netaddr));
	}
	return (FALSE);
}

BOOL
bc_sockaddr_islinklocal(const BCSockAddrS *sockaddr)
{
	BCNetAddrS netaddr;

	if (sockaddr->type.sa.sa_family == AF_INET ||
		sockaddr->type.sa.sa_family == AF_INET6)
	{
		bc_netaddr_fromsockaddr(&netaddr, sockaddr);
		return (bc_netaddr_islinklocal(&netaddr));
	}
	return (FALSE);
}

BOOL
bc_sockaddr_isbroadcast(const BCSockAddrS *sockaddr)
{
	BCNetAddrS netaddr;

	if (sockaddr->type.sa.sa_family == AF_INET)
	{
		bc_netaddr_fromsockaddr(&netaddr, sockaddr);
		return (bc_netaddr_isbroadcast(&netaddr));
	}
	return (FALSE);
	return FALSE;
}

BOOL
bc_sockaddr_isloopback(const BCSockAddrS *sockaddr)
{
	BCNetAddrS netaddr;

	if (sockaddr->type.sa.sa_family == AF_INET ||
		sockaddr->type.sa.sa_family == AF_INET6)
	{
		bc_netaddr_fromsockaddr(&netaddr, sockaddr);
		return (bc_netaddr_isloopback(&netaddr));
	}
	return (FALSE);
}

BOOL
bc_sockaddr_iswildcard(const BCSockAddrS *sockaddr)
{
	BCNetAddrS netaddr;

	if (sockaddr->type.sa.sa_family == AF_INET ||
		sockaddr->type.sa.sa_family == AF_INET6)
	{
		bc_netaddr_fromsockaddr(&netaddr, sockaddr);
		return (bc_netaddr_iswildcard(&netaddr));
	}
	return (FALSE);
}

BCRESULT
bc_sockaddr_frompath(BCSockAddrS *sockaddr, const char *path)
{
#ifdef BC_PLATFORM_HAVESYSUNH
	if (strlen(path) >= sizeof(sockaddr->type.sunix.sun_path))
		return (BC_R_NOSPACE);
	memset(sockaddr, 0, sizeof(*sockaddr));
	sockaddr->length = sizeof(sockaddr->type.sunix);
	sockaddr->type.sunix.sun_family = AF_UNIX;
#ifdef BC_PLATFORM_HAVESALEN
	sockaddr->type.sunix.sun_len =
	    (unsigned char)sizeof(sockaddr->type.sunix);
#endif
	strcpy(sockaddr->type.sunix.sun_path, path);
	return (BC_R_SUCCESS);
#else
	UNUSED(sockaddr);
	UNUSED(path);
	return (BC_R_NOTIMPLEMENTED);
#endif
}

const void *
bc_sockaddr_getaddr(const BCSockAddrS *sockaddr)
{
	const void *pAddr = NULL;

	switch (sockaddr->type.sa.sa_family)
	{
#ifdef BC_PLATFORM_HAVESYSUNH
	case AF_UNIX:
		pAddr = &sockaddr->type.sunix.sun_path;
		break;
#endif // BC_PLATFORM_HAVESYSUNH
	case AF_INET:
		pAddr = &sockaddr->type.sin.sin_addr;
		break;
	case AF_INET6:
		pAddr = &sockaddr->type.sin6.sin6_addr;
		break;
	default:
		LogFatal(_LOCAL_, "unknown address family: %d",
		            (int)sockaddr->type.sa.sa_family);
	}

	return (pAddr);
}

///////////////////////////////////////////////////////////////////////////////
// End of namespace : BC
///////////////////////////////////////////////////////////////////////////////

}

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
