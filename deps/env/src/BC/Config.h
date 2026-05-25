
#ifndef BC_CONFIG_INCLUDED__
#define BC_CONFIG_INCLUDED__

#include "build/build_config.h"
#include "BC/Platform.h"

#ifdef OS_WIN

#include <WinSock2.h>
#include <MSWSock.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include <BC/base/atomic_ref_count.h>

#include <io.h>
#include <errno.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <bitset>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cassert>
#include <cstdarg>
#include <cmath>
#include <wchar.h>
#include <fcntl.h>

#pragma warning(disable:4251)		// DLL Exports Class use none dll exports classes

typedef signed char				int8_t;
typedef unsigned char			uint8_t;
typedef short int				int16_t;
typedef unsigned short			uint16_t;
typedef int						int32_t;
typedef unsigned int			uint32_t;
typedef __int64					int64_t;
typedef unsigned __int64		uint64_t;


typedef float					float32_t;
typedef double					float64_t;
typedef Base::AtomicRefCount	bc_atomic_t;

#else // !OS_WIN

#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <stdarg.h>
#include <string.h>
#include <assert.h>
#include <ctype.h>
#include <wchar.h>
#include <fcntl.h>
#include <sys/time.h>
#include <limits.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#ifdef BC_PLATFORM_NEEDNETINETIN6H
#include <netinet/in6.h>
#endif
#ifdef BC_PLATFORM_NEEDNETINET6IN6H
#include <netinet6/in6.h>
#endif


#undef FAR
#undef  NEAR
#define far
#define near
#define FAR                 
#define NEAR                
#ifndef CONST
#define CONST               const
#endif

#define PASCAL
#define WINAPI
#define wxSIZE_T_IS_UINT

#define _W64

#if defined(_WIN64)
typedef int64_t INT_PTR, *PINT_PTR;
typedef uint64_t UINT_PTR, *PUINT_PTR;

typedef int64_t LONG_PTR, *PLONG_PTR;
typedef uint64_t ULONG_PTR, *PULONG_PTR;

#define __int3264   int64_t

#else
typedef _W64 int INT_PTR, *PINT_PTR;
typedef _W64 unsigned int UINT_PTR, *PUINT_PTR;

typedef _W64 long LONG_PTR, *PLONG_PTR;
typedef _W64 unsigned long ULONG_PTR, *PULONG_PTR;

#define __int3264         int32_t

#endif

typedef unsigned long       DWORD;
typedef bool                BOOL;
typedef unsigned char       BYTE;
typedef unsigned short      WORD;
typedef float               FLOAT;
typedef FLOAT               *PFLOAT;
typedef BOOL near           *PBOOL;
typedef BOOL far            *LPBOOL;
typedef BYTE near           *PBYTE;
typedef BYTE far            *LPBYTE;
typedef int near            *PINT;
typedef int far             *LPINT;
typedef WORD near           *PWORD;
typedef WORD far            *LPWORD;
typedef long far            *LPLONG;
typedef DWORD near          *PDWORD;
typedef DWORD far           *LPDWORD;
typedef void far            *LPVOID;
typedef CONST void far      *LPCVOID;
typedef const BYTE 			*LPCBYTE;

typedef int                 INT;
typedef unsigned int        UINT;
typedef unsigned int        *PUINT;
typedef int64_t				LONGLONG;
typedef uint64_t			ULONGLONG;
typedef ULONG_PTR			DWORD_PTR;
typedef long				LONG;


typedef float				float32_t;
typedef double				float64_t;
typedef int32_t				bc_atomic_t;

#ifndef _LPDWORD_DEFINED
#define _LPDWORD_DEFINED
typedef DWORD *LPDWORD;

#endif // !_LPDWORD_DEFINED
typedef char CHAR;

typedef /* [string] */ CHAR *LPSTR;

typedef /* [string] */ const CHAR *LPCSTR;

#ifndef _WCHAR_DEFINED
#define _WCHAR_DEFINED
typedef wchar_t WCHAR;

#endif // !_WCHAR_DEFINED

#ifdef UNICODE

typedef WCHAR TCHAR;

#else // !UNICODE

typedef CHAR	TCHAR;

#endif // UNICODE

typedef /* [string] */ WCHAR *LPWSTR;

typedef /* [string] */ TCHAR *LPTSTR;

typedef /* [string] */ const WCHAR *LPCWSTR;

typedef /* [string] */ const TCHAR *LPCTSTR;

/* Types use for passing & returning polymorphic values */
typedef UINT_PTR            WPARAM;
typedef LONG_PTR            LPARAM;
typedef LONG_PTR            LRESULT;

#define MAKEWORD(a, b)      ((WORD)(((BYTE)((DWORD_PTR)(a) & 0xff)) | ((WORD)((BYTE)((DWORD_PTR)(b) & 0xff))) << 8))
#define MAKELONG(a, b)      ((LONG)(((WORD)((DWORD_PTR)(a) & 0xffff)) | ((DWORD)((WORD)((DWORD_PTR)(b) & 0xffff))) << 16))
#define LOWORD(l)           ((WORD)((DWORD_PTR)(l) & 0xffff))
#define HIWORD(l)           ((WORD)((DWORD_PTR)(l) >> 16))
#define LOBYTE(w)           ((BYTE)((DWORD_PTR)(w) & 0xff))
#define HIBYTE(w)           ((BYTE)((DWORD_PTR)(w) >> 8))


#ifndef TRUE
#define TRUE		1
#endif // TRUE

#ifndef FALSE
#define FALSE		0
#endif // FALSE

#define IN
#define OUT
#define FORCEINLINE	inline
#define __cdecl
#define MAX_PATH		256

#ifdef UNICODE
#define _tcslen		wcslen
#else // !UNICODE
#define _tcslen		strlen
#endif // UNICODE

#define INFINITE		0xFFFFFFFF
#define memzero(x, y)		memset((x), 0, (y))

#define _T(x)			(x)

#endif // OS_WIN

#define USE_MEMORY_POOL

#ifndef DECLARE_NO_COPY_CLASS


#ifndef BCMAX
#define BCMAX(a,b)            (((a) > (b)) ? (a) : (b))
#endif

#ifndef BCMIN
#define BCMIN(a,b)            (((a) < (b)) ? (a) : (b))
#endif


// ---------------------------------------------------------------------------
// macro to define a class without copy ctor nor assignment operator
// ---------------------------------------------------------------------------

#define DECLARE_NO_COPY_CLASS(classname)        \
	private:                                    \
	classname(const classname&);            \
	classname& operator=(const classname&);
#endif

#ifndef ALIGN_SIZE
#define ALIGN_SIZE(val, align_size)		((val+align_size-1)/align_size*align_size)
#endif


#ifndef RUNTIME_CHECK
#define RUNTIME_CHECK(exp)		\
{bool success = (exp);ASSERT(success);}
#endif // RUNTIME_CHECK


//forces the compiler to declare but not define evil copy constructor
//and assignment operator
#define BC_FORCE_BY_REF_ONLY(cls)	cls(const cls&); const cls& operator = (const cls&)
//boolean value to string
#define BC_VAL_OF_BOOL(b)	(b?"TRUE":"FALSE")
//a safe way to get the value of a C-Style char* string
#define BC_GET_CSTR_VAL(cstr)		(cstr==0?"":cstr)
//just a macro to check for null before deleting a pointer
#define BC_SAFE_DELETE_PTR(ptr)		if(ptr){ delete ptr; ptr = 0; }
//same as ptr delete but for arrays
#define BC_SAFE_DELETE_ARRAY(ptr)	if(ptr){ delete[] ptr; ptr = 0; }

#define BC_UNUSED_ARG(x)	(x)

#define BC_VERSION_MAJOR	1
#define BC_VERSION_MINOR	0
#define BC_VERSION_BUILD	9
#define BC_VERSION_BUILD2	0

#define BC_MAKE_CONTROL_VERSION(major,minor,build,build2)	( (build2 % 100) + (build % 100) * 100 + (minor % 100) * 10000 + (major % 100) * 1000000)

// Efficiently returns the least power of two >= X...
# define BC_POW(X) (((X) == 0)?1:(X-=1,X|=X>>1,X|=X>>2,X|=X>>4,X|=X>>8,X|=X>>16,(++X)))
# define BC_EVEN(NUM) (((NUM) & 1) == 0)
# define BC_ODD(NUM) (((NUM) & 1) == 1)
# define BC_BIT_ENABLED(WORD, BIT) (((WORD) & (BIT)) != 0)
# define BC_BIT_DISABLED(WORD, BIT) (((WORD) & (BIT)) == 0)
# define BC_BIT_CMP_MASK(WORD, BIT, MASK) (((WORD) & (BIT)) == MASK)
# define BC_SET_BITS(WORD, BITS) (WORD |= (BITS))
# define BC_CLR_BITS(WORD, BITS) (WORD &= ~(BITS))

#ifdef _WIN32
//win32 defines
typedef DWORD		dword_t;
typedef HANDLE		handle_t;
typedef SOCKET		socket_t;
typedef SOCKADDR_IN	sockaddr_in_t;
typedef SOCKADDR	sockaddr_t;
#endif // _WIN32


/*%
* Use this to hide unused function arguments.
* \code
* int
* foo(char *bar)
* {
*	UNUSED(bar);
* }
* \endcode
*/
#ifndef UNUSED
//#if defined(__GNUC__)
//#	define UNUSED(x) UNUSED_ ## x __attribute__((unused))
//#else // !__GNUC__
#	define UNUSED(x)      (void)(x)
//#endif // __GNUC__
#endif // UNUSED

typedef unsigned int		BCRESULT;

#define BC_R_SUCCESS			0	/*%< success */
#define BC_R_NOMEMORY			1	/*%< out of memory */
#define BC_R_TIMEDOUT			2	/*%< timed out */
#define BC_R_NOTHREADS			3	/*%< no available threads */
#define BC_R_ADDRNOTAVAIL		4	/*%< address not available */
#define BC_R_ADDRINUSE			5	/*%< address in use */
#define BC_R_NOPERM			    6	/*%< permission denied */
#define BC_R_NOCONN				7	/*%< no pending connections */
#define BC_R_NETUNREACH			8	/*%< network unreachable */
#define BC_R_HOSTUNREACH		9	/*%< host unreachable */
#define BC_R_NETDOWN			10	/*%< network down */
#define BC_R_HOSTDOWN			11	/*%< host down */
#define BC_R_CONNREFUSED		12	/*%< connection refused */
#define BC_R_NORESOURCES		13	/*%< not enough free resources */
#define BC_R_EOF				14	/*%< end of file */
#define BC_R_BOUND				15	/*%< socket already bound */
#define BC_R_RELOAD				16	/*%< reload */
#define BC_R_SUSPEND	      BC_R_RELOAD	/*%< alias of 'reload' */
#define BC_R_LOCKBUSY			17	/*%< lock busy */
#define BC_R_EXISTS				18	/*%< already exists */
#define BC_R_NOSPACE			19	/*%< ran out of space */
#define BC_R_CANCELED			20	/*%< operation canceled */
#define BC_R_NOTBOUND			21	/*%< socket is not bound */
#define BC_R_SHUTTINGDOWN		22	/*%< shutting down */
#define BC_R_NOTFOUND			23	/*%< not found */
#define BC_R_UNEXPECTEDEND		24	/*%< unexpected end of input */
#define BC_R_FAILURE			25	/*%< generic failure */
#define BC_R_IOERROR			26	/*%< I/O error */
#define BC_R_NOTIMPLEMENTED		27	/*%< not implemented */
#define BC_R_UNBALANCED			28	/*%< unbalanced parentheses */
#define BC_R_NOMORE				29	/*%< no more */
#define BC_R_INVALIDFILE		30	/*%< invalid file */
#define BC_R_BADBASE64			31	/*%< bad base64 encoding */
#define BC_R_UNEXPECTEDTOKEN	32	/*%< unexpected token */
#define BC_R_QUOTA				33	/*%< quota reached */
#define BC_R_UNEXPECTED			34	/*%< unexpected error */
#define BC_R_ALREADYRUNNING		35	/*%< already running */
#define BC_R_IGNORE				36	/*%< ignore */
#define BC_R_MASKNONCONTIG      37	/*%< addr mask not contiguous */
#define BC_R_FILENOTFOUND		38	/*%< file not found */
#define BC_R_FILEEXISTS			39	/*%< file already exists */
#define BC_R_NOTCONNECTED		40	/*%< socket is not connected */
#define BC_R_RANGE				41	/*%< out of range */
#define BC_R_NOENTROPY			42	/*%< out of entropy */
#define BC_R_MULTICAST			43	/*%< invalid use of multicast */
#define BC_R_NOTFILE			44	/*%< not a file */
#define BC_R_NOTDIRECTORY		45	/*%< not a directory */
#define BC_R_QUEUEFULL			46	/*%< queue is full */
#define BC_R_FAMILYMISMATCH		47	/*%< address family mismatch */
#define BC_R_FAMILYNOSUPPORT	48	/*%< AF not supported */
#define BC_R_BADHEX				49	/*%< bad hex encoding */
#define BC_R_TOOMANYOPENFILES	50	/*%< too many open files */
#define BC_R_NOTBLOCKING		51	/*%< not blocking */
#define BC_R_UNBALANCEDQUOTES	52	/*%< unbalanced quotes */
#define BC_R_INPROGRESS			53	/*%< operation in progress */
#define BC_R_CONNECTIONRESET	54	/*%< connection reset */
#define BC_R_SOFTQUOTA			55	/*%< soft quota reached */
#define BC_R_BADNUMBER			56	/*%< not a valid number */
#define BC_R_DISABLED			57	/*%< disabled */
#define BC_R_MAXSIZE			58	/*%< max size */
#define BC_R_BADADDRESSFORM		59	/*%< invalid address format */
#define BC_R_BADBASE32			60	/*%< bad base32 encoding */
#define BC_R_DBERROR			61  /*%< database error */
#define BC_R_INVALIDPTR			62  /*%< invalid pointer */
#define BC_R_INVALIDARG			63  /*%< invalid arguments */

/*% Not a result code: the number of results. */
#define BC_R_NRESULTS 			64


/* stderror */
#define BC_STRERRORSIZE			128

#define memcpy2				memcpy
#define memset2				memset
#define memzero(x, y)		memset((x), 0, (y))

#define WANT_IPV6


#endif // BC_CONFIG_INCLUDED__
