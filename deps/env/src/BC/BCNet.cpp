
#include <BC/Utils.h>
#include <BC/BCSockAddr.h>
#include <BC/BCLog.h>
#include <BC/BCNet.h>



// BC_LANG_BEGINDECLS

// #if _MSC_VER < 1300
// ///////////////////////////////////////////////////////////////////////////////
// // ipv6
// ///////////////////////////////////////////////////////////////////////////////

// const struct in6_addr bc_in6addr_any =	IN6ADDR_ANY_INIT;

// const struct in6_addr bc_in6addr_loopback = IN6ADDR_LOOPBACK_INIT;

// #endif

// BC_LANG_ENDDECLS

///////////////////////////////////////////////////////////////////////////////
// Namespace : BC
///////////////////////////////////////////////////////////////////////////////

namespace BC
{

///////////////////////////////////////////////////////////////////////////////
// net
///////////////////////////////////////////////////////////////////////////////

/*%
 * Definitions about UDP port range specification.  This is a total mess of
 * portability variants: some use sysctl (but the sysctl names vary), some use
 * system-specific interfaces, some have the same interface for IPv4 and IPv6,
 * some separate them, etc...
 */

/*%
 * The last resort defaults: use all non well known port space
 */
#ifndef BC_NET_PORTRANGELOW
#define BC_NET_PORTRANGELOW 1024
#endif	/* BC_NET_PORTRANGELOW */
#ifndef BC_NET_PORTRANGEHIGH
#define BC_NET_PORTRANGEHIGH 65535
#endif	/* BC_NET_PORTRANGEHIGH */

#ifdef _WIN32

#if defined(BC_PLATFORM_HAVEIPV6) && defined(BC_PLATFORM_NEEDIN6ADDRANY)
const struct in6_addr bc_net_in6addrany = IN6ADDR_ANY_INIT;
#endif

static BCOnceS 	once = BC_ONCE_INIT;
static BCOnceS 	once_ipv6only = BC_ONCE_INIT;
static BCOnceS 	once_ipv6pktinfo = BC_ONCE_INIT;
static BCRESULT	ipv4_result = BC_R_NOTFOUND;
static BCRESULT	ipv6_result = BC_R_NOTFOUND;
static BCRESULT	ipv6only_result = BC_R_NOTFOUND;
static BCRESULT	ipv6pktinfo_result = BC_R_NOTFOUND;



static BCOnceS initialise_once = BC_ONCE_INIT;
static BOOL initialised = FALSE;

static void
initialiseWinSock(void*)
{
	WORD wVersionRequested;
	WSADATA wsaData;
	int err;

	/* Need Winsock 2.2 or better */
	wVersionRequested = MAKEWORD(2, 2);

	err = WSAStartup(wVersionRequested, &wsaData);
	if (err != 0)
	{
		char strbuf[BC_STRERRORSIZE];
		bc_strerror(err, strbuf, sizeof(strbuf));
		LogFatal(_LOCAL_, "WSAStartup(): %s", strbuf);
		exit(1);
	}

	initialised = TRUE;
}

/*
 * Initialize socket services
 */
void
InitSockets(void)
{
	BCRESULT result;

	result = bc_once_do(&initialise_once, initialiseWinSock, NULL);
	if (result != BC_R_SUCCESS)
	{
		exit(1);
	}
	if (!initialised)
	{
		exit(1);
	}
}

static BCRESULT
try_proto(int domain)
{
	SOCKET s;
	BCRESULT result = BC_R_SUCCESS;
	int errval;

	s = socket(domain, SOCK_STREAM, IPPROTO_TCP);
	if (s == INVALID_SOCKET)
	{
		errval = WSAGetLastError();
		switch (errval)
		{
		case WSAEAFNOSUPPORT:
		case WSAEPROTONOSUPPORT:
		case WSAEINVAL:
			return (BC_R_NOTFOUND);
		default:
			LogError(_LOCAL_, "Unexcepted error occured!");
			return (BC_R_UNEXPECTED);
		}
	}

	closesocket(s);

	return (BC_R_SUCCESS);
}

static void
initialize_action(void*)
{
	InitSockets();
	ipv4_result = try_proto(PF_INET);
#ifdef BC_PLATFORM_HAVEIPV6
#ifdef WANT_IPV6
#ifdef BC_PLATFORM_HAVEIN6PKTINFO
	ipv6_result = try_proto(PF_INET6);
#endif
#endif
#endif
}

static void
initialize(void)
{
	BCRESULT result;

	result = bc_once_do(&once, initialize_action, NULL);
	ASSERT(result == BC_R_SUCCESS);
}

BCRESULT
bc_net_probeipv4(void)
{
	initialize();
	return (ipv4_result);
}

BCRESULT
bc_net_probeipv6(void)
{
	initialize();
	return (ipv6_result);
}

BCRESULT
bc_net_probeunix(void)
{
	return (BC_R_NOTFOUND);
}

#ifdef BC_PLATFORM_HAVEIPV6
#ifdef WANT_IPV6
static void
try_ipv6only(void*)
{
#ifdef IPV6_V6ONLY
	SOCKET s;
	int on;
	//char strbuf[BC_STRERRORSIZE];
#endif
	BCRESULT result;

	result = bc_net_probeipv6();
	if (result != BC_R_SUCCESS)
	{
		ipv6only_result = result;
		return;
	}

#ifndef IPV6_V6ONLY
	ipv6only_result = BC_R_NOTFOUND;
	return;
#else
	/* check for TCP sockets */
	s = socket(PF_INET6, SOCK_STREAM, 0);
	if (s == INVALID_SOCKET)
	{
		LogError(_LOCAL_, "Unexcepted error occured!");
		ipv6only_result = BC_R_UNEXPECTED;
		return;
	}

	on = 1;
	if (setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, (char *)&on, sizeof(on)) < 0)
	{
		ipv6only_result = BC_R_NOTFOUND;
		goto close;
	}

	closesocket(s);

	/* check for UDP sockets */
	s = socket(PF_INET6, SOCK_DGRAM, 0);
	if (s == INVALID_SOCKET)
	{
		LogError(_LOCAL_, "Unexcepted error occured!");
		ipv6only_result = BC_R_UNEXPECTED;
		return;
	}

	on = 1;
	if (setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, (char*)&on, sizeof(on)) < 0)
	{
		ipv6only_result = BC_R_NOTFOUND;
		goto close;
	}

	ipv6only_result = BC_R_SUCCESS;

close:
	closesocket(s);
	return;
#endif /* IPV6_V6ONLY */
}

static void
initialize_ipv6only(void)
{
	BCRESULT result;

	result = bc_once_do(&once_ipv6only,  try_ipv6only, NULL);
	ASSERT(result == BC_R_SUCCESS);
}

static void
try_ipv6pktinfo(void*)
{
	SOCKET s;
	int on;
	char strbuf[BC_STRERRORSIZE];
	BCRESULT result;
	int optname;

	result = bc_net_probeipv6();
	if (result != BC_R_SUCCESS)
	{
		ipv6pktinfo_result = result;
		return;
	}

	/* we only use this for UDP sockets */
	s = socket(PF_INET6, SOCK_DGRAM, IPPROTO_UDP);
	if (s == INVALID_SOCKET)
	{
		bc_strerror(errno, strbuf, sizeof(strbuf));
		LogError(_LOCAL_, "socket() %s: %s", "failed", strbuf);
		ipv6pktinfo_result = BC_R_UNEXPECTED;
		return;
	}

#ifdef IPV6_RECVPKTINFO
	optname = IPV6_RECVPKTINFO;
#else
	optname = IPV6_PKTINFO;
#endif
	on = 1;
	if (setsockopt(s, IPPROTO_IPV6, optname, (const char *) &on,
	               sizeof(on)) < 0)
	{
		ipv6pktinfo_result = BC_R_NOTFOUND;
		goto close;
	}

	ipv6pktinfo_result = BC_R_SUCCESS;

close:
	closesocket(s);
	return;
}

static void
initialize_ipv6pktinfo(void)
{
	BCRESULT result;

	result = bc_once_do(&once_ipv6pktinfo, try_ipv6pktinfo, NULL);
	ASSERT(result == BC_R_SUCCESS);
}
#endif /* WANT_IPV6 */
#endif /* BC_PLATFORM_HAVEIPV6 */

BCRESULT
bc_net_probe_ipv6only(void)
{
#ifdef BC_PLATFORM_HAVEIPV6
#ifdef WANT_IPV6
	initialize_ipv6only();
#else
	ipv6only_result = BC_R_NOTFOUND;
#endif
#endif
	return (ipv6only_result);
}

BCRESULT
bc_net_probe_ipv6pktinfo(void)
{
#ifdef BC_PLATFORM_HAVEIPV6
#ifdef WANT_IPV6
	initialize_ipv6pktinfo();
#else
	ipv6pktinfo_result = BC_R_NOTFOUND;
#endif
#endif
	return (ipv6pktinfo_result);
}

BCRESULT
bc_net_getudpportrange(int af, in_port_t *low, in_port_t *high)
{
	int result = BC_R_FAILURE;

	ASSERT(low != NULL && high != NULL);

	UNUSED(af);

	if (result != BC_R_SUCCESS)
	{
		*low = BC_NET_PORTRANGELOW;
		*high = BC_NET_PORTRANGEHIGH;
	}

	return (BC_R_SUCCESS);	/* we currently never fail in this function */
}

#else // !_WIN32


#ifdef HAVE_SYSCTLBYNAME

/*%
 * sysctl variants
 */
#if defined(__FreeBSD__) || defined(__APPLE__) || defined(__DragonFly__)
#define USE_SYSCTL_PORTRANGE
#define SYSCTL_V4PORTRANGE_LOW	"net.inet.ip.portrange.hifirst"
#define SYSCTL_V4PORTRANGE_HIGH	"net.inet.ip.portrange.hilast"
#define SYSCTL_V6PORTRANGE_LOW	"net.inet.ip.portrange.hifirst"
#define SYSCTL_V6PORTRANGE_HIGH	"net.inet.ip.portrange.hilast"
#endif

#ifdef __NetBSD__
#define USE_SYSCTL_PORTRANGE
#define SYSCTL_V4PORTRANGE_LOW	"net.inet.ip.anonportmin"
#define SYSCTL_V4PORTRANGE_HIGH	"net.inet.ip.anonportmax"
#define SYSCTL_V6PORTRANGE_LOW	"net.inet6.ip6.anonportmin"
#define SYSCTL_V6PORTRANGE_HIGH	"net.inet6.ip6.anonportmax"
#endif

#else /* !HAVE_SYSCTLBYNAME */

#ifdef __OpenBSD__
#define USE_SYSCTL_PORTRANGE
#define SYSCTL_V4PORTRANGE_LOW	{ CTL_NET, PF_INET, IPPROTO_IP, \
				  IPCTL_IPPORT_HIFIRSTAUTO }
#define SYSCTL_V4PORTRANGE_HIGH	{ CTL_NET, PF_INET, IPPROTO_IP, \
				  IPCTL_IPPORT_HILASTAUTO }
/* Same for IPv6 */
#define SYSCTL_V6PORTRANGE_LOW	SYSCTL_V4PORTRANGE_LOW
#define SYSCTL_V6PORTRANGE_HIGH	SYSCTL_V4PORTRANGE_HIGH
#endif

#endif /* HAVE_SYSCTLBYNAME */

#if defined(BC_PLATFORM_HAVEIPV6)
# if defined(BC_PLATFORM_NEEDIN6ADDRANY)
const struct in6_addr bc_net_in6addrany = IN6ADDR_ANY_INIT;
# endif

# if defined(BC_PLATFORM_NEEDIN6ADDRLOOPBACK)
const struct in6_addr bc_net_in6addrloop = IN6ADDR_LOOPBACK_INIT;
# endif

# if defined(WANT_IPV6)
static BCOnceS 	once_ipv6only = BC_ONCE_INIT;
# endif

# if defined(BC_PLATFORM_HAVEIN6PKTINFO)
static BCOnceS 	once_ipv6pktinfo = BC_ONCE_INIT;
# endif
#endif /* BC_PLATFORM_HAVEIPV6 */

static BCOnceS 	once = BC_ONCE_INIT;

static BCRESULT	ipv4_result = BC_R_NOTFOUND;
static BCRESULT	ipv6_result = BC_R_NOTFOUND;
static BCRESULT	unix_result = BC_R_NOTFOUND;
static BCRESULT	ipv6only_result = BC_R_NOTFOUND;
static BCRESULT	ipv6pktinfo_result = BC_R_NOTFOUND;

static BCRESULT
try_proto(int domain) {
	int s;
	BCRESULT result = BC_R_SUCCESS;
	char strbuf[BC_STRERRORSIZE];

	s = socket(domain, SOCK_STREAM, 0);
	if (s == -1) {
		switch (errno) {
#ifdef EAFNOSUPPORT
		case EAFNOSUPPORT:
#endif
#ifdef EPROTONOSUPPORT
		case EPROTONOSUPPORT:
#endif
#ifdef EINVAL
		case EINVAL:
#endif
			return (BC_R_NOTFOUND);
		default:
			bc_strerror(errno, strbuf, sizeof(strbuf));
			LogError(_LOCAL_,  "Unexcepted error occur : %s", strbuf);
			return (BC_R_UNEXPECTED);
		}
	}

#ifdef BC_PLATFORM_HAVEIPV6
#ifdef WANT_IPV6
#ifdef BC_PLATFORM_HAVEIN6PKTINFO
	if (domain == PF_INET6) {
		struct sockaddr_in6 sin6;
		unsigned int len;

		/*
		 * Check to see if IPv6 is broken, as is common on Linux.
		 */
		len = sizeof(sin6);
		if (getsockname(s, (struct sockaddr *)&sin6, (socklen_t *)(void *)&len) < 0)
		{
			LogError(_LOCAL_,  "retrieving the address of an IPv6 "
				      "socket from the kernel failed.");
			LogError(_LOCAL_,  "IPv6 is not supported.");
			result = BC_R_NOTFOUND;
		} else {
			if (len == sizeof(struct sockaddr_in6))
				result = BC_R_SUCCESS;
			else {
				LogError(_LOCAL_,  "IPv6 structures in kernel and "
					      "user space do not match.");
				LogError(_LOCAL_,  "IPv6 is not supported.");
				result = BC_R_NOTFOUND;
			}
		}
	}
#endif
#endif
#endif

	(void)close(s);

	return (result);
}

static void
initialize_action(void*) {
	ipv4_result = try_proto(PF_INET);
#ifdef BC_PLATFORM_HAVEIPV6
#ifdef WANT_IPV6
#ifdef BC_PLATFORM_HAVEIN6PKTINFO
	ipv6_result = try_proto(PF_INET6);
#endif
#endif
#endif
#ifdef BC_PLATFORM_HAVESYSUNH
	unix_result = try_proto(PF_UNIX);
#endif
}

static void
initialize(void)
{
	BCRESULT result;

	result = bc_once_do(&once, initialize_action, NULL);
	ASSERT(result == BC_R_SUCCESS);
}

BCRESULT
bc_net_probeipv4(void) {
	initialize();
	return (ipv4_result);
}

BCRESULT
bc_net_probeipv6(void) {
	initialize();
	return (ipv6_result);
}

BCRESULT
bc_net_probeunix(void) {
	initialize();
	return (unix_result);
}

#ifdef BC_PLATFORM_HAVEIPV6
#ifdef WANT_IPV6
static void
try_ipv6only(void*) {
#ifdef IPV6_V6ONLY
	int s, on;
	char strbuf[BC_STRERRORSIZE];
#endif
	BCRESULT result;

	result = bc_net_probeipv6();
	if (result != BC_R_SUCCESS) {
		ipv6only_result = result;
		return;
	}

#ifndef IPV6_V6ONLY
	ipv6only_result = BC_R_NOTFOUND;
	return;
#else
	/* check for TCP sockets */
	s = socket(PF_INET6, SOCK_STREAM, 0);
	if (s == -1) {
		bc_strerror(errno, strbuf, sizeof(strbuf));
		LogError(_LOCAL_, "Unexcepted error occur : %s\n", strbuf);
		ipv6only_result = BC_R_UNEXPECTED;
		return;
	}

	on = 1;
	if (setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &on, sizeof(on)) < 0) {
		ipv6only_result = BC_R_NOTFOUND;
		goto close;
	}

	close(s);

	/* check for UDP sockets */
	s = socket(PF_INET6, SOCK_DGRAM, 0);
	if (s == -1) {
		bc_strerror(errno, strbuf, sizeof(strbuf));
		LogError(_LOCAL_, "Unexcepted error occur: %s\n", strbuf);
		ipv6only_result = BC_R_UNEXPECTED;
		return;
	}

	on = 1;
	if (setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &on, sizeof(on)) < 0) {
		ipv6only_result = BC_R_NOTFOUND;
		goto close;
	}

	close(s);

	ipv6only_result = BC_R_SUCCESS;

close:
	close(s);
	return;
#endif /* IPV6_V6ONLY */
}

static void
initialize_ipv6only(void)
{
	BCRESULT result;

	result = bc_once_do(&once_ipv6only,  try_ipv6only, NULL);
	ASSERT(result == BC_R_SUCCESS);
}
#endif /* WANT_IPV6 */

#ifdef BC_PLATFORM_HAVEIN6PKTINFO
static void
try_ipv6pktinfo(void*) {
	int s, on;
	char strbuf[BC_STRERRORSIZE];
	BCRESULT result;
	int optname;

	result = bc_net_probeipv6();
	if (result != BC_R_SUCCESS) {
		ipv6pktinfo_result = result;
		return;
	}

	/* we only use this for UDP sockets */
	s = socket(PF_INET6, SOCK_DGRAM, IPPROTO_UDP);
	if (s == -1) {
		bc_strerror(errno, strbuf, sizeof(strbuf));
		LogError(_LOCAL_, "Unexcepted error occur : %s\n", strbuf);
		ipv6pktinfo_result = BC_R_UNEXPECTED;
		return;
	}

#ifdef IPV6_RECVPKTINFO
	optname = IPV6_RECVPKTINFO;
#else
	optname = IPV6_PKTINFO;
#endif
	on = 1;
	if (setsockopt(s, IPPROTO_IPV6, optname, &on, sizeof(on)) < 0) {
		ipv6pktinfo_result = BC_R_NOTFOUND;
		goto close;
	}

	close(s);
	ipv6pktinfo_result = BC_R_SUCCESS;

close:
	close(s);
	return;
}

static void
initialize_ipv6pktinfo(void)
{
	BCRESULT result;

	result = bc_once_do(&once_ipv6pktinfo, try_ipv6pktinfo, NULL);
	ASSERT(result == BC_R_SUCCESS);
}
#endif /* BC_PLATFORM_HAVEIN6PKTINFO */
#endif /* BC_PLATFORM_HAVEIPV6 */

BCRESULT
bc_net_probe_ipv6only(void) {
#ifdef BC_PLATFORM_HAVEIPV6
#ifdef WANT_IPV6
	initialize_ipv6only();
#else
	ipv6only_result = BC_R_NOTFOUND;
#endif
#endif
	return (ipv6only_result);
}

BCRESULT
bc_net_probe_ipv6pktinfo(void) {
#ifdef BC_PLATFORM_HAVEIPV6
#ifdef BC_PLATFORM_HAVEIN6PKTINFO
#ifdef WANT_IPV6
	initialize_ipv6pktinfo();
#else
	ipv6pktinfo_result = BC_R_NOTFOUND;
#endif
#endif
#endif
	return (ipv6pktinfo_result);
}

#if defined(USE_SYSCTL_PORTRANGE)
#if defined(HAVE_SYSCTLBYNAME)
static BCRESULT
getudpportrange_sysctl(int af, in_port_t *low, in_port_t *high) {
	int port_low, port_high;
	size_t portlen;
	const char *sysctlname_lowport, *sysctlname_hiport;

	if (af == AF_INET) {
		sysctlname_lowport = SYSCTL_V4PORTRANGE_LOW;
		sysctlname_hiport = SYSCTL_V4PORTRANGE_HIGH;
	} else {
		sysctlname_lowport = SYSCTL_V6PORTRANGE_LOW;
		sysctlname_hiport = SYSCTL_V6PORTRANGE_HIGH;
	}
	portlen = sizeof(portlen);
	if (sysctlbyname(sysctlname_lowport, &port_low, &portlen,
			 NULL, 0) < 0) {
		return (BC_R_FAILURE);
	}
	portlen = sizeof(portlen);
	if (sysctlbyname(sysctlname_hiport, &port_high, &portlen,
			 NULL, 0) < 0) {
		return (BC_R_FAILURE);
	}
	if ((port_low & ~0xffff) != 0 || (port_high & ~0xffff) != 0)
		return (BC_R_RANGE);

	*low = (in_port_t)port_low;
	*high = (in_port_t)port_high;

	return (BC_R_SUCCESS);
}
#else /* !HAVE_SYSCTLBYNAME */
static BCRESULT
getudpportrange_sysctl(int af, in_port_t *low, in_port_t *high) {
	int mib_lo4[4] = SYSCTL_V4PORTRANGE_LOW;
	int mib_hi4[4] = SYSCTL_V4PORTRANGE_HIGH;
	int mib_lo6[4] = SYSCTL_V6PORTRANGE_LOW;
	int mib_hi6[4] = SYSCTL_V6PORTRANGE_HIGH;
	int *mib_lo, *mib_hi, miblen;
	int port_low, port_high;
	size_t portlen;

	if (af == AF_INET) {
		mib_lo = mib_lo4;
		mib_hi = mib_hi4;
		miblen = sizeof(mib_lo4) / sizeof(mib_lo4[0]);
	} else {
		mib_lo = mib_lo6;
		mib_hi = mib_hi6;
		miblen = sizeof(mib_lo6) / sizeof(mib_lo6[0]);
	}

	portlen = sizeof(portlen);
	if (sysctl(mib_lo, miblen, &port_low, &portlen, NULL, 0) < 0) {
		return (BC_R_FAILURE);
	}

	portlen = sizeof(portlen);
	if (sysctl(mib_hi, miblen, &port_high, &portlen, NULL, 0) < 0) {
		return (BC_R_FAILURE);
	}

	if ((port_low & ~0xffff) != 0 || (port_high & ~0xffff) != 0)
		return (BC_R_RANGE);

	*low = (in_port_t) port_low;
	*high = (in_port_t) port_high;

	return (BC_R_SUCCESS);
}
#endif /* HAVE_SYSCTLBYNAME */
#endif /* USE_SYSCTL_PORTRANGE */

BCRESULT
bc_net_getudpportrange(int af, in_port_t *low, in_port_t *high) {
	int result = BC_R_FAILURE;

	REQUIRE(low != NULL && high != NULL);

#if defined(USE_SYSCTL_PORTRANGE)
	result = getudpportrange_sysctl(af, low, high);
#else
	UNUSED(af);
#endif

	if (result != BC_R_SUCCESS) {
		*low = BC_NET_PORTRANGELOW;
		*high = BC_NET_PORTRANGEHIGH;
	}

	return (BC_R_SUCCESS);	/* we currently never fail in this function */
}

#endif // _WIN32

void
bc_net_disableipv4(void)
{
	initialize();
	if (ipv4_result == BC_R_SUCCESS)
		ipv4_result = BC_R_DISABLED;
}

void
bc_net_disableipv6(void)
{
	initialize();
	if (ipv6_result == BC_R_SUCCESS)
		ipv6_result = BC_R_DISABLED;
}

void
bc_net_enableipv4(void)
{
	initialize();
	if (ipv4_result == BC_R_DISABLED)
		ipv4_result = BC_R_SUCCESS;
}

void
bc_net_enableipv6(void)
{
	initialize();
	if (ipv6_result == BC_R_DISABLED)
		ipv6_result = BC_R_SUCCESS;
}

///////////////////////////////////////////////////////////////////////////////
// class : netaddr
///////////////////////////////////////////////////////////////////////////////


BOOL
bc_netaddr_equal(const BCNetAddrS *a, const BCNetAddrS *b)
{
	ASSERT(a != NULL && b != NULL);

	if (a->family != b->family)
		return (FALSE);

	if (a->zone != b->zone)
		return (FALSE);

	switch (a->family)
	{
	case AF_INET:
		if (a->type.in.s_addr != b->type.in.s_addr)
			return (FALSE);
		break;
	case AF_INET6:
		if (memcmp(&a->type.in6, &b->type.in6,
		           sizeof(a->type.in6)) != 0 ||
		        a->zone != b->zone)
			return (FALSE);
		break;
#ifdef BC_PLATFORM_HAVESYSUNH
	case AF_UNIX:
		if (strcmp(a->type.un, b->type.un) != 0)
			return (FALSE);
		break;
#endif
	default:
		return (FALSE);
	}
	return (TRUE);
}

BOOL
bc_netaddr_eqprefix(const BCNetAddrS *a, const BCNetAddrS *b,
                     unsigned int prefixlen)
{
	const unsigned char *pa, *pb;
	unsigned int ipabytes; /* Length of whole IP address in bytes */
	unsigned int nbytes;   /* Number of significant whole bytes */
	unsigned int nbits;    /* Number of significant leftover bits */

	ASSERT(a != NULL && b != NULL);

	if (a->family != b->family)
		return (FALSE);

	if (a->zone != b->zone && b->zone != 0)
		return (FALSE);

	switch (a->family)
	{
	case AF_INET:
		pa = (const unsigned char *) &a->type.in;
		pb = (const unsigned char *) &b->type.in;
		ipabytes = 4;
		break;
	case AF_INET6:
		pa = (const unsigned char *) &a->type.in6;
		pb = (const unsigned char *) &b->type.in6;
		ipabytes = 16;
		break;
	default:
		pa = pb = NULL; /* Avoid silly compiler warning. */
		ipabytes = 0; /* Ditto. */
		return (FALSE);
	}

	/*
	 * Don't crash if we get a pattern like 10.0.0.1/9999999.
	 */
	if (prefixlen > ipabytes * 8)
		prefixlen = ipabytes * 8;

	nbytes = prefixlen / 8;
	nbits = prefixlen % 8;

	if (nbytes > 0)
	{
		if (memcmp(pa, pb, nbytes) != 0)
			return (FALSE);
	}
	if (nbits > 0)
	{
		unsigned int bytea, byteb, mask;
		ASSERT(nbytes < ipabytes);
		ASSERT(nbits < 8);
		bytea = pa[nbytes];
		byteb = pb[nbytes];
		mask = (0xFF << (8-nbits)) & 0xFF;
		if ((bytea & mask) != (byteb & mask))
			return (FALSE);
	}
	return (TRUE);
}

BCRESULT
bc_netaddr_totext(const BCNetAddrS *netaddr, BCPString *target)
{
	char abuf[sizeof("xxxx:xxxx:xxxx:xxxx:xxxx:xxxx:255.255.255.255")];
	char zbuf[sizeof("%4294967295")];
	unsigned int alen;
	int zlen;
	const char *r;
	const void *type;

	ASSERT(netaddr != NULL);

	switch (netaddr->family)
	{
	case AF_INET:
		type = &netaddr->type.in;
		break;
	case AF_INET6:
		type = &netaddr->type.in6;
		break;
#ifdef BC_PLATFORM_HAVESYSUNH
	case AF_UNIX:
		alen = strlen(netaddr->type.un);
		target->append(netaddr->type.un, alen);
		return (BC_R_SUCCESS);
#endif
	default:
		return (BC_R_FAILURE);
	}
	r = inet_ntop(netaddr->family, type, abuf, sizeof(abuf));
	if (r == NULL)
		return (BC_R_FAILURE);

	alen = strlen(abuf);
	ASSERT(alen < sizeof(abuf));

	zlen = 0;
	if (netaddr->family == AF_INET6 && netaddr->zone != 0)
	{
		zlen = snprintf(zbuf, sizeof(zbuf), "%%%u", netaddr->zone);
		if (zlen < 0)
			return (BC_R_FAILURE);
		ASSERT((unsigned int)zlen < sizeof(zbuf));
	}

	target->append(abuf, alen);
	target->append(zbuf, zlen);

	return (BC_R_SUCCESS);
}

void
bc_netaddr_format(const BCNetAddrS *na, char *array, unsigned int size)
{
	BCRESULT result;
	BCPString buf;

	result = bc_netaddr_totext(na, &buf);

	/*
	 * Null terminate.
	 */
	if (result == BC_R_SUCCESS)
	{
		if (buf.length() >= 1)
		{
			uint32_t nLen;
			buf.append((char)0);
			nLen = buf.length();
			memcpy(array, buf.c_str(), nLen > size?size:nLen);
		}
		else
		{
			result = BC_R_NOSPACE;
		}
	}

	if (result != BC_R_SUCCESS)
	{
		snprintf(array, size, "<unknown address, family %u>", na->family);
		array[size - 1] = '\0';
	}
}


BCRESULT
bc_netaddr_prefixok(const BCNetAddrS *na, unsigned int prefixlen)
{
	static const unsigned char zeros[16] = {0};
	unsigned int nbits, nbytes, ipbytes;
	const unsigned char *p;

	switch (na->family)
	{
	case AF_INET:
		p = (const unsigned char *) &na->type.in;
		ipbytes = 4;
		if (prefixlen > 32)
			return (BC_R_RANGE);
		break;
	case AF_INET6:
		p = (const unsigned char *) &na->type.in6;
		ipbytes = 16;
		if (prefixlen > 128)
			return (BC_R_RANGE);
		break;
	default:
		ipbytes = 0;
		return (BC_R_NOTIMPLEMENTED);
	}
	nbytes = prefixlen / 8;
	nbits = prefixlen % 8;
	if (nbits != 0)
	{
		if ((p[nbytes] & (0xff>>nbits)) != 0U)
			return (BC_R_FAILURE);
		nbytes++;
	}
	if (memcmp(p + nbytes, zeros, ipbytes - nbytes) != 0)
		return (BC_R_FAILURE);
	return (BC_R_SUCCESS);
}

BCRESULT
bc_netaddr_masktoprefixlen(const BCNetAddrS *s, unsigned int *lenp)
{
	unsigned int nbits, nbytes, ipbytes, i;
	const unsigned char *p;

	switch (s->family)
	{
	case AF_INET:
		p = (const unsigned char *) &s->type.in;
		ipbytes = 4;
		break;
	case AF_INET6:
		p = (const unsigned char *) &s->type.in6;
		ipbytes = 16;
		break;
	default:
		ipbytes = 0;
		return (BC_R_NOTIMPLEMENTED);
	}
	nbytes = nbits = 0;
	for (i = 0; i < ipbytes; i++)
	{
		if (p[i] != 0xFF)
			break;
	}
	nbytes = i;
	if (i < ipbytes)
	{
		unsigned int c = p[nbytes];
		while ((c & 0x80) != 0 && nbits < 8)
		{
			c <<= 1;
			nbits++;
		}
		if ((c & 0xFF) != 0)
			return (BC_R_MASKNONCONTIG);
		i++;
	}
	for (; i < ipbytes; i++)
	{
		if (p[i] != 0)
			return (BC_R_MASKNONCONTIG);
	}
	*lenp = nbytes * 8 + nbits;
	return (BC_R_SUCCESS);
}

void
bc_netaddr_fromin(BCNetAddrS *netaddr, const struct in_addr *ina)
{
	memset(netaddr, 0, sizeof(*netaddr));
	netaddr->family = AF_INET;
	netaddr->type.in = *ina;
}

void
bc_netaddr_fromin6(BCNetAddrS *netaddr, const struct in6_addr *ina6)
{
	memset(netaddr, 0, sizeof(*netaddr));
	netaddr->family = AF_INET6;
	netaddr->type.in6 = *ina6;
}

BCRESULT
bc_netaddr_frompath(BCNetAddrS *netaddr, const char *path)
{
#ifdef BC_PLATFORM_HAVESYSUNH
	if (strlen(path) > sizeof(netaddr->type.un) - 1)
		return (BC_R_NOSPACE);

	memset(netaddr, 0, sizeof(*netaddr));
	netaddr->family = AF_UNIX;
	strcpy(netaddr->type.un, path);
	netaddr->zone = 0;
	return (BC_R_SUCCESS);
#else
	UNUSED(netaddr);
	UNUSED(path);
	return (BC_R_NOTIMPLEMENTED);
#endif
}


void
bc_netaddr_setzone(BCNetAddrS *netaddr, uint32_t zone)
{
	/* we currently only support AF_INET6. */
	ASSERT(netaddr->family == AF_INET6);

	netaddr->zone = zone;
}

uint32_t
bc_netaddr_getzone(const BCNetAddrS *netaddr)
{
	return (netaddr->zone);
}

void
bc_netaddr_fromsockaddr(BCNetAddrS *t, const BCSockAddrS *s)
{
	int family = s->type.sa.sa_family;
	t->family = family;
	switch (family)
	{
	case AF_INET:
		t->type.in = s->type.sin.sin_addr;
		t->zone = 0;
		break;
	case AF_INET6:
		memcpy(&t->type.in6, &s->type.sin6.sin6_addr, 16);
#ifdef BC_PLATFORM_HAVESCOPEID
		t->zone = s->type.sin6.sin6_scope_id;
#else
		t->zone = 0;
#endif
		break;
#ifdef BC_PLATFORM_HAVESYSUNH
	case AF_UNIX:
		memcpy(t->type.un, s->type.sunix.sun_path, sizeof(t->type.un));
		t->zone = 0;
		break;
#endif
	default:
		ASSERT(0);
	}
}

void
bc_netaddr_any(BCNetAddrS *netaddr)
{
	memset(netaddr, 0, sizeof(*netaddr));
	netaddr->family = AF_INET;
	netaddr->type.in.s_addr = INADDR_ANY;
}

void
bc_netaddr_any6(BCNetAddrS *netaddr)
{
	memset(netaddr, 0, sizeof(*netaddr));
	netaddr->family = AF_INET6;
	netaddr->type.in6 = in6addr_any;
}

BOOL
bc_netaddr_ismulticast(BCNetAddrS *na)
{
	switch (na->family)
	{
	case AF_INET:
		return (BC_IPADDR_ISMULTICAST(na->type.in.s_addr));
	case AF_INET6:
		return (IN6_IS_ADDR_MULTICAST(&na->type.in6));
	default:
		return (FALSE);  /* XXXMLG ? */
	}
}

BOOL
bc_netaddr_isexperimental(BCNetAddrS *na)
{
	switch (na->family)
	{
	case AF_INET:
		return (BC_IPADDR_ISEXPERIMENTAL(na->type.in.s_addr));
	default:
		return (FALSE);  /* XXXMLG ? */
	}
}

BOOL
bc_netaddr_islinklocal(BCNetAddrS *na)
{
	switch (na->family)
	{
	case AF_INET:
		return (BC_IPADDR_ISLINKLOCAL(na->type.in.s_addr));
	case AF_INET6:
		return (IN6_IS_ADDR_LINKLOCAL(&na->type.in6));
	default:
		return (FALSE);
	}
}

BOOL
bc_netaddr_issitelocal(BCNetAddrS *na)
{
	switch (na->family)
	{
	case AF_INET:
		return (BC_IPADDR_ISSITELOCAL(na->type.in.s_addr));
	case AF_INET6:
		return (IN6_IS_ADDR_SITELOCAL(&na->type.in6));
	default:
		return (FALSE);
	}
}

BOOL
bc_netaddr_isbroadcast(BCNetAddrS *na)
{
	switch (na->family)
	{
	case AF_INET:
		return (BC_IPADDR_ISBROADCAST(na->type.in.s_addr));
	default:
		return (FALSE);
	}
}

BOOL
bc_netaddr_isloopback(BCNetAddrS *na)
{
	switch (na->family)
	{
	case AF_INET:
		return (BC_IPADDR_ISLOOPBACK(na->type.in.s_addr));
	case AF_INET6:
		return (IN6_IS_ADDR_LOOPBACK(&na->type.in6));
	default:
		return (FALSE);
	}
}

BOOL
bc_netaddr_iswildcard(BCNetAddrS *na)
{
	switch (na->family)
	{
	case AF_INET:
		return (BC_IPADDR_ISWILDCARD(na->type.in.s_addr));
	case AF_INET6:
		return (IN6_IS_ADDR_WILDCARD(&na->type.in6));
	default:
		return (FALSE);
	}
}

void
bc_netaddr_fromv4mapped(BCNetAddrS *t, const BCNetAddrS *s)
{
	BCNetAddrS *src;

	DE_CONST_TYPE(s, src, BCNetAddrS *);	/* Must come before IN6_IS_ADDR_V4MAPPED. */

	ASSERT(s->family == AF_INET6);
	ASSERT(IN6_IS_ADDR_V4MAPPED(&src->type.in6));

	memset(t, 0, sizeof(*t));
	t->family = AF_INET;
	memcpy(&t->type.in, (char *)&src->type.in6 + 12, 4);
	return;
}

///////////////////////////////////////////////////////////////////////////////
// inet_aton
///////////////////////////////////////////////////////////////////////////////

/*%
 * Check whether "cp" is a valid ascii representation
 * of an Internet address and convert to a binary address.
 * Returns 1 if the address is valid, 0 if not.
 * This replaces inet_addr, the return value from which
 * cannot distinguish between failure and a local broadcast address.
 */
int
bc_net_aton(const char *cp, struct in_addr *addr) {
	unsigned long val;
	int base, n;
	unsigned char c;
	uint8_t parts[4];
	uint8_t *pp = parts;
	int digit;

	c = *cp;
	for (;;) {
		/*
		 * Collect number up to ``.''.
		 * Values are specified as for C:
		 * 0x=hex, 0=octal, isdigit=decimal.
		 */
		if (!isdigit(c & 0xff))
			return (0);
		val = 0; base = 10; digit = 0;
		if (c == '0') {
			c = *++cp;
			if (c == 'x' || c == 'X')
				base = 16, c = *++cp;
			else {
				base = 8;
				digit = 1;
			}
		}
		for (;;) {
			/*
			 * isascii() is valid for all integer values, and
			 * when it is true, c is known to be in scope
			 * for isdigit().  No cast necessary.  Similar
			 * comment applies for later ctype uses.
			 */
			if (isascii(c) && isdigit(c)) {
				if (base == 8 && (c == '8' || c == '9'))
					return (0);
				val = (val * base) + (c - '0');
				c = *++cp;
				digit = 1;
			} else if (base == 16 && isascii(c) && isxdigit(c)) {
				val = (val << 4) |
					(c + 10 - (islower(c) ? 'a' : 'A'));
				c = *++cp;
				digit = 1;
			} else
				break;
		}
		if (c == '.') {
			/*
			 * Internet format:
			 *	a.b.c.d
			 *	a.b.c	(with c treated as 16 bits)
			 *	a.b	(with b treated as 24 bits)
			 */
			if (pp >= parts + 3 || val > 0xffU)
				return (0);
			*pp++ = (uint8_t)val;
			c = *++cp;
		} else
			break;
	}
	/*
	 * Check for trailing characters.
	 */
	if (c != '\0' && (!isascii(c) || !isspace(c)))
		return (0);
	/*
	 * Did we get a valid digit?
	 */
	if (!digit)
		return (0);
	/*
	 * Concoct the address according to
	 * the number of parts specified.
	 */
	n = pp - parts + 1;
	switch (n) {
	case 1:				/* a -- 32 bits */
		break;

	case 2:				/* a.b -- 8.24 bits */
		if (val > 0xffffffU)
			return (0);
		val |= parts[0] << 24;
		break;

	case 3:				/* a.b.c -- 8.8.16 bits */
		if (val > 0xffffU)
			return (0);
		val |= (parts[0] << 24) | (parts[1] << 16);
		break;

	case 4:				/* a.b.c.d -- 8.8.8.8 bits */
		if (val > 0xffU)
			return (0);
		val |= (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8);
		break;
	}
	if (addr != NULL)
		addr->s_addr = htonl(val);

	return (1);
}

///////////////////////////////////////////////////////////////////////////////
// inet_ntop
///////////////////////////////////////////////////////////////////////////////


#define NS_INT16SZ	 2
#define NS_IN6ADDRSZ	16

/*
 * WARNING: Don't even consider trying to compile this on a system where
 * sizeof(int) < 4.  sizeof(int) > 4 is fine; all the world's not a VAX.
 */

static const char *inet_ntop4(const unsigned char *src, char *dst,
			      size_t size);

#ifdef AF_INET6
static const char *inet_ntop6(const unsigned char *src, char *dst,
			      size_t size);
#endif

/*! char *
 * bc_net_ntop(af, src, dst, size)
 *	convert a network format address to presentation format.
 * \return
 *	pointer to presentation format address (`dst'), or NULL (see errno).
 * \author
 *	Paul Vixie, 1996.
 */
const char *
bc_net_ntop(int af, const void *src, char *dst, size_t size)
{
	switch (af) {
	case AF_INET:
		return (inet_ntop4((const uint8_t *)src, dst, size));
#ifdef AF_INET6
	case AF_INET6:
		return (inet_ntop6((const uint8_t *)src, dst, size));
#endif
	default:
		errno = EAFNOSUPPORT;
		return (NULL);
	}
	/* NOTREACHED */
}

/*! const char *
 * inet_ntop4(src, dst, size)
 *	format an IPv4 address
 * \return
 *	`dst' (as a const)
 * \note
 *	(1) uses no statics
 * \note
 *	(2) takes a unsigned char* not an in_addr as input
 * \author
 *	Paul Vixie, 1996.
 */
static const char *
inet_ntop4(const unsigned char *src, char *dst, size_t size)
{
	static const char *fmt = "%u.%u.%u.%u";
	char tmp[sizeof("255.255.255.255")];

	if ((size_t)sprintf(tmp, fmt, src[0], src[1], src[2], src[3]) >= size)
	{
		errno = ENOSPC;
		return (NULL);
	}
	strcpy(dst, tmp);

	return (dst);
}

/*! const char *
 * bc_inet_ntop6(src, dst, size)
 *	convert IPv6 binary address into presentation (printable) format
 * \author
 *	Paul Vixie, 1996.
 */
#ifdef AF_INET6
static const char *
inet_ntop6(const unsigned char *src, char *dst, size_t size)
{
	/*
	 * Note that int32_t and int16_t need only be "at least" large enough
	 * to contain a value of the specified size.  On some systems, like
	 * Crays, there is no such thing as an integer variable with 16 bits.
	 * Keep this in mind if you think this function should have been coded
	 * to use pointer overlays.  All the world's not a VAX.
	 */
	char tmp[sizeof("ffff:ffff:ffff:ffff:ffff:ffff:255.255.255.255")], *tp;
	struct { int base, len; } best, cur;
	unsigned int words[NS_IN6ADDRSZ / NS_INT16SZ];
	int i;

	/*
	 * Preprocess:
	 *	Copy the input (bytewise) array into a wordwise array.
	 *	Find the longest run of 0x00's in src[] for :: shorthanding.
	 */
    memset(&best, 0, sizeof(best));
    memset(&cur, 0, sizeof(cur));
	memset(words, '\0', sizeof(words));
	for (i = 0; i < NS_IN6ADDRSZ; i++)
		words[i / 2] |= (src[i] << ((1 - (i % 2)) << 3));
	best.base = -1;
	cur.base = -1;
	for (i = 0; i < (NS_IN6ADDRSZ / NS_INT16SZ); i++) {
		if (words[i] == 0) {
			if (cur.base == -1)
				cur.base = i, cur.len = 1;
			else
				cur.len++;
		} else {
			if (cur.base != -1) {
				if (best.base == -1 || cur.len > best.len)
					best = cur;
				cur.base = -1;
			}
		}
	}
	if (cur.base != -1) {
		if (best.base == -1 || cur.len > best.len)
			best = cur;
	}
	if (best.base != -1 && best.len < 2)
		best.base = -1;

	/*
	 * Format the result.
	 */
	tp = tmp;
	for (i = 0; i < (NS_IN6ADDRSZ / NS_INT16SZ); i++) {
		/* Are we inside the best run of 0x00's? */
		if (best.base != -1 && i >= best.base &&
		    i < (best.base + best.len)) {
			if (i == best.base)
				*tp++ = ':';
			continue;
		}
		/* Are we following an initial run of 0x00s or any real hex? */
		if (i != 0)
			*tp++ = ':';
		/* Is this address an encapsulated IPv4? */
		if (i == 6 && best.base == 0 && (best.len == 6 ||
		    (best.len == 7 && words[7] != 0x0001) ||
		    (best.len == 5 && words[5] == 0xffff))) {
			if (!inet_ntop4(src+12, tp,
					sizeof(tmp) - (tp - tmp)))
				return (NULL);
			tp += strlen(tp);
			break;
		}
		tp += sprintf(tp, "%x", words[i]);
	}
	/* Was it a trailing run of 0x00's? */
	if (best.base != -1 && (best.base + best.len) ==
	    (NS_IN6ADDRSZ / NS_INT16SZ))
		*tp++ = ':';
	*tp++ = '\0';

	/*
	 * Check for overflow, copy, and we're done.
	 */
	if ((size_t)(tp - tmp) > size) {
		errno = ENOSPC;
		return (NULL);
	}
	strcpy(dst, tmp);
	return (dst);
}
#endif /* AF_INET6 */

///////////////////////////////////////////////////////////////////////////////
// inet_pton
///////////////////////////////////////////////////////////////////////////////


/*% INT16 Size */
#define NS_INT16SZ	 2
/*% IPv4 Address Size */
#define NS_INADDRSZ	 4
/*% IPv6 Address Size */
#define NS_IN6ADDRSZ	16

/*
 * WARNING: Don't even consider trying to compile this on a system where
 * sizeof(int) < 4.  sizeof(int) > 4 is fine; all the world's not a VAX.
 */

static int inet_pton4(const char *src, unsigned char *dst);
static int inet_pton6(const char *src, unsigned char *dst);

/*%
 *	convert from presentation format (which usually means ASCII printable)
 *	to network format (which is usually some kind of binary format).
 * \return
 *	1 if the address was valid for the specified address family
 *	0 if the address wasn't valid (`dst' is untouched in this case)
 *	-1 if some other error occurred (`dst' is untouched in this case, too)
 * \author
 *	Paul Vixie, 1996.
 */
int
bc_net_pton(int af, const char *src, void *dst) {
	switch (af) {
	case AF_INET:
		return (inet_pton4((const char *)src, (uint8_t *)dst));
	case AF_INET6:
		return (inet_pton6((const char *)src, (uint8_t *)dst));
	default:
		errno = EAFNOSUPPORT;
		return (-1);
	}
	/* NOTREACHED */
}

/*!\fn static int inet_pton4(const char *src, unsigned char *dst)
 * \brief
 *	like inet_aton() but without all the hexadecimal and shorthand.
 * \return
 *	1 if `src' is a valid dotted quad, else 0.
 * \note
 *	does not touch `dst' unless it's returning 1.
 * \author
 *	Paul Vixie, 1996.
 */
static int
inet_pton4(const char *src, unsigned char *dst) {
	static const char digits[] = "0123456789";
	int saw_digit, octets, ch;
	unsigned char tmp[NS_INADDRSZ], *tp;

	saw_digit = 0;
	octets = 0;
	*(tp = tmp) = 0;
	while ((ch = *src++) != '\0') {
		const char *pch;

		if ((pch = strchr(digits, ch)) != NULL) {
			unsigned int new_ = *tp * 10 + (pch - digits);

			if (saw_digit && *tp == 0)
				return (0);
			if (new_ > 255)
				return (0);
			*tp = new_;
			if (!saw_digit) {
				if (++octets > 4)
					return (0);
				saw_digit = 1;
			}
		} else if (ch == '.' && saw_digit) {
			if (octets == 4)
				return (0);
			*++tp = 0;
			saw_digit = 0;
		} else
			return (0);
	}
	if (octets < 4)
		return (0);
	memcpy(dst, tmp, NS_INADDRSZ);
	return (1);
}

/*%
 *	convert presentation level address to network order binary form.
 * \return
 *	1 if `src' is a valid [RFC1884 2.2] address, else 0.
 * \note
 *	(1) does not touch `dst' unless it's returning 1.
 * \note
 *	(2) :: in a full address is silently ignored.
 * \author
 *	inspired by Mark Andrews.
 * \author
 *	Paul Vixie, 1996.
 */
static int
inet_pton6(const char *src, unsigned char *dst) {
	static const char xdigits_l[] = "0123456789abcdef",
			  xdigits_u[] = "0123456789ABCDEF";
	unsigned char tmp[NS_IN6ADDRSZ], *tp, *endp, *colonp;
	const char *xdigits, *curtok;
	int ch, seen_xdigits;
	unsigned int val;

	memset((tp = tmp), '\0', NS_IN6ADDRSZ);
	endp = tp + NS_IN6ADDRSZ;
	colonp = NULL;
	/* Leading :: requires some special handling. */
	if (*src == ':')
		if (*++src != ':')
			return (0);
	curtok = src;
	seen_xdigits = 0;
	val = 0;
	while ((ch = *src++) != '\0') {
		const char *pch;

		if ((pch = strchr((xdigits = xdigits_l), ch)) == NULL)
			pch = strchr((xdigits = xdigits_u), ch);
		if (pch != NULL) {
			val <<= 4;
			val |= (pch - xdigits);
			if (++seen_xdigits > 4)
				return (0);
			continue;
		}
		if (ch == ':') {
			curtok = src;
			if (!seen_xdigits) {
				if (colonp)
					return (0);
				colonp = tp;
				continue;
			}
			if (tp + NS_INT16SZ > endp)
				return (0);
			*tp++ = (unsigned char) (val >> 8) & 0xff;
			*tp++ = (unsigned char) val & 0xff;
			seen_xdigits = 0;
			val = 0;
			continue;
		}
		if (ch == '.' && ((tp + NS_INADDRSZ) <= endp) &&
		    inet_pton4(curtok, tp) > 0) {
			tp += NS_INADDRSZ;
			seen_xdigits = 0;
			break;	/* '\0' was seen by inet_pton4(). */
		}
		return (0);
	}
	if (seen_xdigits) {
		if (tp + NS_INT16SZ > endp)
			return (0);
		*tp++ = (unsigned char) (val >> 8) & 0xff;
		*tp++ = (unsigned char) val & 0xff;
	}
	if (colonp != NULL) {
		/*
		 * Since some memmove()'s erroneously fail to handle
		 * overlapping regions, we'll do the shift by hand.
		 */
		const int n = tp - colonp;
		int i;

		if (tp == endp)
			return (0);
		for (i = 1; i <= n; i++) {
			endp[- i] = colonp[n - i];
			colonp[n - i] = 0;
		}
		tp = endp;
	}
	if (tp != endp)
		return (0);
	memcpy(dst, tmp, NS_IN6ADDRSZ);
	return (1);
}

///////////////////////////////////////////////////////////////////////////////
// End of namespace :
///////////////////////////////////////////////////////////////////////////////

}

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
