
#ifndef BC_UTILS_H_INCLUDED__
#define BC_UTILS_H_INCLUDED__

#include <BC/Exports.h>
#include <BC/BCPString.h>
#include <vector>
#include <string>

#define BC_UI8BEI(x) \
	(DWORD)((x)[0])
#define BC_UI16BEI(x) \
	(DWORD)((((DWORD)((x)[0]) & 0xFF) << 8) + \
	         ((DWORD)((x)[1]) & 0xFF))
#define BC_UI24BEI(x) \
	(DWORD)((((DWORD)((x)[0]) & 0xFF) << 16) + \
	        (((DWORD)((x)[1]) & 0xFF) << 8) + \
	         ((DWORD)((x)[2]) & 0xFF))
#define BC_UI32BEI(x) \
	(DWORD)((((DWORD)((x)[0]) & 0xFF) << 24) + \
	        (((DWORD)((x)[1]) & 0xFF) << 16) + \
	        (((DWORD)((x)[2]) & 0xFF) << 8) + \
	         ((DWORD)((x)[3]) & 0xFF))

#define BC_UI8BEO(p, u) \
	((char *)p)[0] = (char)((u & 0x000000FF));
#define BC_UI16BEO(p, u) \
	((char *)p)[0] = (char)((u & 0x0000FF00) >> 8);\
	((char *)p)[1] = (char)((u & 0x000000FF));
#define BC_UI24BEO(p, u) \
	((char *)p)[0] = (char)((u & 0x00FF0000) >> 16);\
	((char *)p)[1] = (char)((u & 0x0000FF00) >> 8);\
	((char *)p)[2] = (char)((u & 0x000000FF));
#define BC_UI32BEO(p, u) \
	((char *)p)[0] = (char)((u & 0xFF000000) >> 24);\
	((char *)p)[1] = (char)((u & 0x00FF0000) >> 16);\
	((char *)p)[2] = (char)((u & 0x0000FF00) >> 8);\
	((char *)p)[3] = (char)((u & 0x000000FF));

typedef struct BCRECT {
	int			x;         ///< top left corner  of pict
	int			y;         ///< top left corner  of pict
	uint32_t	w;         ///< width            of pict
	uint32_t	h;         ///< height           of pict
	BCRECT(int x_ = 0, int y_ = 0, uint32_t w_ = 0, uint32_t h_ = 0)
		: x(x_), y(y_), w(w_), h(h_){}
}BCRECT;

#ifndef S_ISDIR
# define S_ISDIR(m)	(((m) & S_IFMT) == S_IFDIR)
#endif
#ifndef S_ISREG
# define S_ISREG(m)	(((m) & S_IFMT) == S_IFREG)
#endif

#define CHAR2BYTE(x) ((x)<=0x39?(x-0x30):((x)<0x61?(x)-0x37:(x)-0x57))
#define HEX2BYTE(h,l) (CHAR2BYTE(h)<<4|CHAR2BYTE(l))

inline bool IsHexChar(char chr)
{
	if (chr >= 0x30 && chr <= 0x39) return true;
	if (chr >= 0x41 && chr <= 0x46) return true;
	if (chr >= 0x61 && chr <= 0x66) return true;
	return false;
}

#if defined(__WIN32__) || defined(_WIN32) || defined(_QNX4)
#define _strncasecmp _strnicmp
#define snprintf _snprintf
#define strncasecmp _strnicmp
#define strcasecmp _stricmp
#else
#define _strncasecmp strncasecmp
#endif

#define SIZE_TO_SHIFT(shift) ((shift) < 0 ? 0: 1<<(shift))



int32_t zcmp(int32_t i1, int32_t i2);
int32_t zcmp(double d1, double d2);
int32_t zcmp(const int8_t * s1, const int8_t *s2);

template<typename _Key>
int32_t	zcmp(_Key k1, _Key k2);
int32_t initKs(int32_t);
double  initKs(double d);
const int8_t * initKs(const int8_t *);

//template<class KS>
//KS initKs(KS & k);

template<typename KS>
KS initKs(KS & k)
{
	UNUSED(k);
	return KS();
}

template<typename _Type>
void initTypeArray(void *pArray, size_t nCount)
{
	_Type *pType = (_Type *)pArray;
	for (size_t i = 0; i < nCount; i++)
	{
		new (pType + i)_Type();
	}
}

template<typename _Type>
void uninitTypeArray(void *pArray, size_t nCount)
{
	_Type *pType = (_Type *)pArray;
	for (size_t i = 0; i < nCount; i++)
	{
		(pType + i)->~_Type();
	}
}

inline int32_t zcmp(int32_t i1, int32_t i2)
{
	if (i1 > i2)
	{
		return 1;
	}
	else if (i1 < i2)
	{
		return -1;
	}
	else
	{
		return 0;
	}
}

inline int32_t zcmp(double d1, double d2)
{
	if (d1 > d2)
	{
		return 1;
	}
	else if (d1 < d2)
	{
		return -1;
	}
	else
	{
		return 0;
	}
}

inline int32_t zcmp(const char * s1, const char *s2)
{

	int32_t a = strcmp(s1, s2);

	return a;
}

template<class _Key>
int32_t zcmp(_Key k1, _Key k2)
{
	uint8_t * b1 = (uint8_t*)&k1;
	uint8_t * b2 = (uint8_t*)&k2;
	for (size_t i = 0; i < sizeof(_Key); i++, b1++, b2++)
	{
		if (*b1 > *b2)
		{
			return 1;
		}
		else if (*b1 < *b2)
		{
			return -1;
		}
	}
	return 0;
}

inline int32_t initKs(int32_t)
{
	return 0;
}

inline double initKs(double d)
{
	UNUSED(d);
	return 0.0;
}

inline const char * initKs(const char *)
{
	return "";
}

///////////////////////////////////////////////////////////////////////////////
// Namespace : 
///////////////////////////////////////////////////////////////////////////////

namespace BC
{


typedef struct BCOnceS {
	int32_t		status;
#if defined(_WIN32) || defined(__GNUC__)
	bc_atomic_t		counter;
#else // !_WIN32
#if defined(__ANDROID__)
#	if __ANDROID_API__ >= 21
	std::atomic<int32_t>	counter;
#	else 
	int32_t		counter;
#	endif
#endif // __ANDROID__
#endif // _WIN32
	BCOnceS(int32_t status_, int32_t init_counter) 
		: status(status_), counter(init_counter) {}
} BCOnceS;

#define BC_ONCE_INIT_NEEDED		0
#define BC_ONCE_INIT_DONE		1

#define BC_ONCE_INIT { BC_ONCE_INIT_NEEDED, 1 }

/* Count the number of elements in an array. The array must be defined
 * as such; using this with a dynamically allocated array will give
 * incorrect results.
 */
#define BC_N_ELEMENTS(arr)		(sizeof (arr) / sizeof ((arr)[0]))

BC_API
BCRESULT
bc_once_do(BCOnceS *controller, void(*lpfn)(void*), void* args);

/*%
* Use this to remove the const qualifier of a variable to assign it to
* a non-const variable or pass it as a non-const function argument ...
* but only when you are sure it won't then be changed!
* This is necessary to sometimes shut up some compilers
* (as with gcc -Wcast-qual) when there is just no other good way to avoid the
* situation.
*/
#define DE_CONST(konst, var) \
	do { \
		union { const void *k; void *v; } _u; \
		_u.k = konst; \
		var = _u.v; \
	} while (0)

#define DE_CONST_TYPE(konst, var, type_) \
	do { \
		union { const void *k; void *v; } _u; \
		_u.k = konst; \
		var = (type_)_u.v; \
	} while (0)



/* region */
typedef struct BCRegionS
{
	uint8_t *	base;
	uint32_t	length;

	BCRegionS(LPCVOID lpData = NULL, size_t size = 0) 
		: base((uint8_t *)lpData), length(size) {}
	~BCRegionS(){}

	void Reset()
	{
		base = NULL;
		length = 0;
	}
}BCRegionS;

/* strerror */
BC_API
void
bc_strerror(int num, char *buf, size_t size);

#ifdef _WIN32
/* errno2result*/
#define bc_errno2result(posixerrno) \
	bc_errno2resultx(posixerrno, __FILE__, __LINE__)
#else
#define bc_errno2result(posixerrno) \
	bc__errno2resultx(posixerrno)
#endif // _WIN32

BC_API
BCRESULT
bc_errno2resultx(int posixerrno, const char *file, int line);

BC_API
BCRESULT
bc__errno2resultx(int posixerrno);

BC_API
LPCSTR 
bc_result2string(BCRESULT result);

///////////////////////////////////////////////////////////////////////////////
// Macros & typedefs
///////////////////////////////////////////////////////////////////////////////

BC_API
size_t 
HexToString(
	const uint8_t* pBuffer,
	size_t iBytes,
	LPBYTE pOutBuf,
	size_t oBytes);

BC_API
size_t 
StringToHex(
	LPCSTR lpStrIn,
	size_t nStrLen,
	LPBYTE pBuffer,
	size_t nBytes);

BC_API
BCPString 
HexToString(const uint8_t *pBuffer, size_t iBytes);

BC_API
size_t 
StringToHex(
	const BCPString &s,
	uint8_t *pBuffer,
	size_t nBytes);

BC_API bool		bcMakDir(LPCSTR szDir);


//#ifdef _WIN32
//
//BCPString BC_API GetModuleFileName(
//    HINSTANCE hModule = 0);
//
//BCPString BC_API GetUserName();
//
//BCPString BC_API GetFileVersion();
//
//BCPString BC_API GetModuleFilePath(
//	LPCSTR szModuleName = NULL,
//	bool bIncludeModule = true);
//
//#else
//
//int BC_API GetModuleFileName(
//    LPCSTR sModuleName,
//    char* sFileName,
//    int nSize);
//
//int BC_API
//GetExeFullPath(char *lpBuf, int nBufLen);
//
//int BC_API
//GetExeShortPath(const char *szFullPath, char *lpBuf, int nBufLen);
//
//#endif // _WIN32

uint32_t BC_API RandomBuffer(
	void *pBuffer,
	uint32_t nBufLen);
uint32_t BC_API RandomString(
	void *pBuffer,
	uint32_t nBufLen);

BCRESULT 
BC_API
DNSGetAddrInfo(
	LPCSTR lpszHost, 
	void *lpBuffer, 
	size_t nBufSize,
	int32_t &refNetType);

LPCSTR 
BC_API
bc_result2string(BCRESULT result);

uint32_t
BC_API
bc_strlcpy(char *dest, const char *src, uint32_t dest_size);

uint32_t
BC_API
bc_strlcat(char *dest, const char *src, uint32_t dest_size);

int
BC_API
bc_vasprintf(char **str, const char *fmt, va_list args);

int
BC_API
bc_asprintf(char **str, const char *fmt, ...);


void
BC_API
SplitString(
	const ::std::string& str,
	const ::std::string& delimiter,
	::std::vector< ::std::string>* dest);

void
BC_API
SplitStringByFirstDelimiter(
	const ::std::string& str,
	const ::std::string& delimiter,
	::std::vector< ::std::string>* dest);

bool 
BC_API
IsPathAbsolute(const std::string& path);

std::string
BC_API
GetBasePathName(const std::string& path);

void 
BC_API
ShowTraceStack(const char* szBriefInfo);

///////////////////////////////////////////////////////////////////////////////
// End of namespace : BC
///////////////////////////////////////////////////////////////////////////////

} // End of namespace BC

#endif // BC_UTILS_H_INCLUDED__
