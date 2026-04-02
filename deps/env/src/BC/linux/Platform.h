
#ifndef BC_LINUX_PLATFORM_H_INCLUDED__
#define BC_LINUX_PLATFORM_H_INCLUDED__

///////////////////////////////////////////////////////////////////////////////
// Namespace :
///////////////////////////////////////////////////////////////////////////////

/*****
 ***** Platform-dependent defines.
 *****/

#define BC_PLATFORM_USETHREADS	1

/***
 *** Network.
 ***/

#define BC_PLATFORM_HAVEIPV6	1
#if _MSC_VER > 1200
#define BC_PLATFORM_HAVEIN6PKTINFO		1
#define BC_PLATFORM_QUADFORMAT				"I64"
#define BC_PLATFORM_HAVEXADD					1
#define BC_PLATFORM_HAVECMPXCHG			1
#elif defined(__GNUC__)
#define BC_PLATFORM_HAVEIPV6 1
#define BC_PLATFORM_HAVEIN6PKTINFO 1
#define BC_PLATFORM_HAVESCOPEID 1
#define BC_NET_BSD44MSGHDR 1
#define BC_PLATFORM_HAVEIFNAMETOINDEX 1
#if defined(OS_LINUX) || defined(OS_ANDROID)
#define BC_PLATFORM_HAVEEPOLL 1
#define BC_SOCKADDR_LEN_T socklen_t
#elif defined(OS_MAC) || defined(OS_IOS)
#define BC_PLATFORM_HAVEKQUEUE 1
#define BC_PLATFORM_HAVES6ADDR16 1
#define __APPLE_USE_RFC_3542 1
#endif // OS_LINUX
#define BC_PLATFORM_USEBACKTRACE 1
#define BC_PLATFORM_QUADFORMAT "ll"
#define BC_PLATFORM_NEEDSTRLCPY 1
#define BC_PLATFORM_NEEDSTRLCAT 1
#define BC_PLATFORM_USETHREADS 1
#define BC_PLATFORM_RLIMITTYPE rlim_t
#define BC_PLATFORM_HAVELONGLONG 1
#define BC_PLATFORM_HAVESYSUNH 1
#define BC_PLATFORM_HAVEXADD 1
#define BC_PLATFORM_HAVEATOMICSTORE 1
#define BC_PLATFORM_HAVECMPXCHG 1
#define BC_PLATFORM_USEGCCASM 1
#define BC_PLATFORM_HAVESTRINGSH 1
#define BC_PLATFORM_NORETURN_PRE
#define BC_PLATFORM_NORETURN_POST __attribute__((noreturn))
#endif // _MSC_VER
#define BC_PLATFORM_HAVESCOPEID 1
#define BC_PLATFORM_NEEDPORTT
#undef MSG_TRUNC
#define BC_PLATFORM_NEEDNTOP
#define BC_PLATFORM_NEEDPTON

#define BC_PLATFORM_NEEDSTRSEP		1
#define BC_PLATFORM_NEEDSTRLCPY	1
#define BC_PLATFORM_NEEDSTRLCAT	1
#define BC_PLATFORM_NEEDSTRLCPY	1

/*
 * Used to control how extern data is linked; needed for Win32 platforms.
 */
#define BC_PLATFORM_USEDECLSPEC 1

/*
 * Define this here for now as winsock2.h defines h_errno
 * and we don't want to redeclare it.
 */
#define BC_PLATFORM_NONSTDHERRNO



#ifdef WIN32
	#define __Win32__				1
	#define kPlatformNameString     "Win32"

	#define _64BITARG_ "I64"
	#define _S64BITARG_ "I64d"
	#define _U64BITARG_ "I64u"
	#if __LP64__
		#define _S32BITARG_ "d"
		#define _U32BITARG_ "u"
	#else
		#define _S32BITARG_ "ld"
		#define _U32BITARG_ "lu"
	#endif

    /* paths */
    #define kEOLString "\r\n"
    #define kPathDelimiterString "\\"
    #define kPathDelimiterChar '\\'
    #define kPartialPathBeginsWithDelimiter 0
#elif defined(__GNUC__)
	#define kPlatformNameString     "GNU/Linux"

	#if __LP64__
		#define _64BITARG_  "l"
		#define _S64BITARG_ "ld"
		#define _U64BITARG_ "lu"
		#define _LLBITARG_  "ll"
		#define _SLLBITARG_ "lld"
		#define _ULLBITARG_ "llu"
		#define _S32BITARG_ "d"
		#define _U32BITARG_ "u"
	#else
		#define _64BITARG_  "ll"
		#define _S64BITARG_ "lld"
		#define _U64BITARG_ "llu"
		#define _LLBITARG_  "ll"
		#define _SLLBITARG_ "lld"
		#define _ULLBITARG_ "llu"
		#define _S32BITARG_ "d"
		#define _U32BITARG_ "u"
	#endif // __LP64__

    /* paths */
    #define kEOLString "\n"
    #define kPathDelimiterString "/"
    #define kPathDelimiterChar '/'
    #define kPartialPathBeginsWithDelimiter 0
#endif


///////////////////////////////////////////////////////////////////////////////
// End of namespace :
///////////////////////////////////////////////////////////////////////////////

#endif // BC_LINUX_PLATFORM_H_INCLUDED__

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
