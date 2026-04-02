
#ifndef BC_WIN32_PLATFORM_H_INCLUDED__
#define BC_WIN32_PLATFORM_H_INCLUDED__

///////////////////////////////////////////////////////////////////////////////
// Namespace :
///////////////////////////////////////////////////////////////////////////////

/*****
 ***** Platform-dependent defines.
 *****/

#define BC_PLATFORM_USETHREADS

/***
 *** Network.
 ***/

#define BC_PLATFORM_HAVEIPV6
#define BC_PLATFORM_HAVEIN6PKTINFO
#define BC_PLATFORM_HAVESCOPEID
#define BC_PLATFORM_NEEDPORTT
#undef MSG_TRUNC
#define BC_PLATFORM_NEEDNTOP
#define BC_PLATFORM_NEEDPTON

#define BC_PLATFORM_QUADFORMAT "I64"

#define BC_PLATFORM_NEEDSTRSEP
#define BC_PLATFORM_NEEDSTRLCPY
#define BC_PLATFORM_NEEDSTRLCAT
#define BC_PLATFORM_NEEDSTRLCPY

/*
 * Used to control how extern data is linked; needed for Win32 platforms.
 */
#define BC_PLATFORM_USEDECLSPEC 1

/*
 * Define this here for now as winsock2.h defines h_errno
 * and we don't want to redeclare it.
 */
#define BC_PLATFORM_NONSTDHERRNO

/*
 * Define if the platform has <sys/un.h>.
 */
#ifdef _WIN32
#undef BC_PLATFORM_HAVESYSUNH
#else
#define BC_PLATFORM_HAVESYSUNH
#endif


#define BC_PLATFORM_NEEDNETINETIN6H

#define BC_PLATFORM_NEEDNETINET6IN6H

#define BC_PLATFORM_HAVEINADDR6

#define BC_PLATFORM_HAVEIPV6

#ifndef _WIN32
#define BC_PLATFORM_HAVEIN6PKTINFO
#define BC_PLATFORM_HAVESALEN
#endif

#define BC_PLATFORM_HAVEXADD
#define BC_PLATFORM_HAVECMPXCHG


#ifdef _WIN32
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
	#endif

    /* paths */
    #define kEOLString "\n"
    #define kPathDelimiterString "/"
    #define kPathDelimiterChar '/'
    #define kPartialPathBeginsWithDelimiter 0
#endif


///////////////////////////////////////////////////////////////////////////////
// End of namespace :
///////////////////////////////////////////////////////////////////////////////

#endif // BC_WIN32_PLATFORM_H_INCLUDED__

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
