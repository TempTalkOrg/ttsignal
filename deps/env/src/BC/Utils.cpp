
#include <stdarg.h>
#include <BC/BCPString.h>
#include <BC/BCMemPool.h>
#include <BC/base/atomic_ref_count.h>
#ifdef _WIN32
#include <fcntl.h>
#include <direct.h>
#include <sys/stat.h>
#include <Lmcons.h>     // UNLEN
#include <dbghelp.h>
#include <WinSock2.h>
#include <WS2tcpip.h>

#pragma comment(lib, "version.lib")
#pragma comment(lib, "dbghelp.lib")
#else
#include <sys/stat.h>
#include <execinfo.h>
#include <netdb.h>
#endif // _WIN32
#include <BC/Utils.h>
#include <BC/BCTime.h>
#include <BC/BCLog.h>


///////////////////////////////////////////////////////////////////////////////
// Using directives
///////////////////////////////////////////////////////////////////////////////


///////////////////////////////////////////////////////////////////////////////
// Namespace: BC
///////////////////////////////////////////////////////////////////////////////

namespace BC
{

size_t HexToString(
	const uint8_t * pBuffer,
	size_t iBytes,
	LPBYTE pOutBuf,
	size_t oBytes)
{
	LPBYTE lpResult = pOutBuf;
	size_t nOffset = 0;

	for (size_t i = 0; i < iBytes; i++)
	{
		uint8_t c ;

		uint8_t b = pBuffer[i] >> 4;

		if (9 >= b)
		{
			c = b + '0';
		}
		else
		{
			c = (b - 10) + 'A';
		}

		lpResult[nOffset] = (BYTE)c;
		nOffset++;
		if (nOffset >= oBytes)
		{
			return nOffset;
		}

		b = pBuffer[i] & 0x0f;

		if (9 >= b)
		{
			c = b + '0';
		}
		else
		{
			c = (b - 10) + 'A';
		}

		lpResult[nOffset] = (BYTE)c;
		nOffset++;
		if (nOffset >= oBytes)
		{
			return nOffset;
		}
	}

	return nOffset;
}

size_t StringToHex(
	LPCSTR lpStrIn,
	size_t nStrLen,
	LPBYTE pBuffer,
	size_t nBytes)
{
	size_t data_size = 0;
	for (size_t i = 0; i < nBytes; i++)
	{
		const size_t stringOffset = i * 2;
		if (stringOffset >= nStrLen)
		{
			break;
		}
		uint8_t val = 0;

		const uint8_t b = lpStrIn[stringOffset];

		if (isdigit(b))
		{
			val = (uint8_t)((b - '0') * 16);
		}
		else
		{
			val = (uint8_t)(((toupper(b) - 'A') + 10) * 16);
		}

		const uint8_t b1 = lpStrIn[stringOffset + 1];

		if (isdigit(b1))
		{
			val += b1 - '0' ;
		}
		else
		{
			val += (uint8_t)((toupper(b1) - 'A') + 10);
		}

		pBuffer[i] = val;
		data_size++;
	}
	return data_size;
}


BCPString HexToString(
	const uint8_t *pBuffer,
	size_t iBytes)
{
	BCPString result;

	for (size_t i = 0; i < iBytes; i++)
	{
		uint8_t c ;

		uint8_t b = pBuffer[i] >> 4;

		if (9 >= b)
		{
			c = b + '0';
		}
		else
		{
			c = (b - 10) + 'A';
		}

		result += (char)c;

		b = pBuffer[i] & 0x0f;

		if (9 >= b)
		{
			c = b + '0';
		}
		else
		{
			c = (b - 10) + 'A';
		}

		result += (char)c;
	}

	return result;
}

size_t StringToHex(
	const BCPString &s,
	uint8_t *pBuffer,
	size_t nBytes)
{
	size_t data_size = 0;
	for (size_t i = 0; i < nBytes; i++)
	{
		const size_t stringOffset = i * 2;
		if (stringOffset >= s.size())
		{
			break;
		}
		uint8_t val = 0;

		const uint8_t b = s[stringOffset];

		if (isdigit(b))
		{
			val = (uint8_t)((b - '0') * 16);
		}
		else
		{
			val = (uint8_t)(((toupper(b) - 'A') + 10) * 16);
		}

		const uint8_t b1 = s[stringOffset + 1];

		if (isdigit(b1))
		{
			val += b1 - '0' ;
		}
		else
		{
			val += (uint8_t)((toupper(b1) - 'A') + 10);
		}

		pBuffer[i] = val;
		data_size++;
	}
	return data_size;
}

//
//#ifdef _WIN32
//BCPString GetModuleFileName(
//    HINSTANCE hModule /* = 0 */)
//{
//	static bool gotName = false;
//
//	static BCPString name = "UNAVAILABLE";
//
//	if (!gotName)
//	{
//		char moduleFileName[MAX_PATH + 1] ;
//		DWORD moduleFileNameLen = MAX_PATH ;
//
//		if (::GetModuleFileName(hModule, moduleFileName, moduleFileNameLen))
//		{
//			name = moduleFileName;
//		}
//
//		gotName = true;
//	}
//
//	return name;
//}
//
//BCPString GetUserName()
//{
//	static bool gotName = false;
//
//	static BCPString name = "UNAVAILABLE";
//
//	if (!gotName)
//	{
//		char userName[UNLEN + 1] ;
//		DWORD userNameLen = UNLEN;
//
//		if (::GetUserName(userName, &userNameLen))
//		{
//			name = userName;
//		}
//
//		gotName = true;
//	}
//
//	return name;
//}
//
//BCPString GetFileVersion()
//{
//	BCPString version;
//
//	const BCPString moduleFileName = GetModuleFileName(NULL);
//
//	LPTSTR pModuleFileName = const_cast<LPTSTR>(moduleFileName.c_str());
//
//	DWORD zero = 0;
//
//	DWORD verSize = ::GetFileVersionInfoSize(pModuleFileName, &zero);
//
//	if (verSize != 0)
//	{
//		int8_t spBuffer[1024];
//
//		if (::GetFileVersionInfo(pModuleFileName, 0, verSize, spBuffer))
//		{
//			LPTSTR pVersion = 0;
//			UINT verLen = 0;
//
//			if (::VerQueryValue(spBuffer,
//			                    const_cast<LPTSTR>("\\StringFileInfo\\080904b0\\ProductVersion"),
//			                    (void**)&pVersion,
//			                    &verLen))
//			{
//				version = pVersion;
//			}
//		}
//	}
//
//	return version;
//}
//
//BCPString GetModuleFilePath(
//	LPCSTR szModuleName /* = NULL*/,
//	bool bIncludeModule/* = true*/)
//{
//	char szAppName[MAX_PATH + 1] = {0};
//	HMODULE hModule = ::GetModuleHandle(szModuleName);
//	::GetModuleFileName(hModule, szAppName, _MAX_PATH);
//	if (bIncludeModule)
//	{
//		return szAppName;
//	}
//	BCPString sAppName(szAppName);
//	int pos = sAppName.rfind('\\');
//	BCPString sAppPath = sAppName.Left(pos);
//	sAppPath.append("\\");
//	return sAppPath;
//}
//#else // !_WIN32
//
//int GetModuleFileName( LPCSTR sModuleName, char* sFileName, int nSize)
//{
//	int ret = -1;
//	if( strchr( sModuleName,'/' ) != NULL )
//	{
//		strncpy( sFileName, sModuleName, min(strlen(sModuleName),((size_t)(nSize - 1))));
//	}
//	else
//	{
//		char* sPath = getenv("PATH");
//		char* pHead = sPath;
//		char* pTail = NULL;
//		while( pHead != NULL && *pHead != '\x0' )
//		{
//			pTail = strchr( pHead, ':' );
//			if( pTail != NULL )
//			{
//				strncpy( sFileName, pHead, pTail-pHead );
//				sFileName[pTail-pHead] = '\x0';
//				pHead = pTail+1;
//			}
//			else
//			{
//				strcpy( sFileName, pHead );
//				pHead = NULL;
//			}
//
//			int nLen = strlen(sFileName);
//			if( sFileName[nLen] != '/' )
//			{
//				sFileName[nLen] = '/';
//			}
//			strcpy( sFileName+nLen+1,sModuleName);
//			if( 0 == access( sFileName, F_OK ) )
//			{
//				ret = 0;
//				break;
//			}
//		}
//	}
//	return ret;
//}
//
//int GetExeFullPath(char *lpBuf, int nBufLen)
//{
//    int result;
//
//    ASSERT(lpBuf != NULL && nBufLen > 0);
//    result = readlink("/proc/self/exe", lpBuf, nBufLen);
//    if (result < 0 || result >= nBufLen)
//    {
//        return -1;
//    }
//    lpBuf[result] = '\0';
//    return result;
//}
//
//int GetExeShortPath(const char *szFullPath, char *lpBuf, int nBufLen)
//{
//    int result;
//    char szPath[MAX_PATH];
//
//    ASSERT(lpBuf != NULL && nBufLen > 0);
//
//    if (szFullPath == NULL)
//    {
//        result = GetExeFullPath(szPath, MAX_PATH);
//        if (result < 0)
//        {
//            return -1;
//        }
//        szPath[result] = '\0';
//        szFullPath = szPath;
//    }
//    ASSERT(szFullPath);
//
//    const char *pPrev, *pNext;
//
//    pNext = strchr(szFullPath, '/');
//    pPrev = pNext;
//    while (pNext != NULL && *pNext != '\x0')
//    {
//        pPrev = pNext;
//        pNext = strchr(pNext + 1, '/');
//    }
//    if (pPrev && *pPrev != '\x0')
//    {
//        int nLen;
//
//        nLen = pPrev - szFullPath;
//        nLen = nLen > nBufLen?nBufLen:nLen;
//        if (nLen > 0)
//        {
//            strncpy(lpBuf, szFullPath, nLen);
//        }
//        lpBuf[nLen] = '\x0';
//
//        return nLen;
//    }
//    return -1;
//}
//#endif // _WIN32

bool IsBigEndian()
{
	static const uint16_t test_endian = 0x01;
	static const bool bBigEndian = ((const uint8_t *)&test_endian)[0] == 0;
	return bBigEndian;
}


BC_API
BCRESULT 
bc_once_do(BCOnceS *controller, void(*lpfn)(void*), void *args)
{
	ASSERT(controller != NULL && lpfn != NULL);

	if (controller->status == BC_ONCE_INIT_NEEDED) 
	{
		if (!Base::AtomicRefCountDec(&controller->counter))
		{
			if (controller->status == BC_ONCE_INIT_NEEDED)
			{
				lpfn(args);
				controller->status = BC_ONCE_INIT_DONE;
			}
		}
		else
		{
			while (controller->status == BC_ONCE_INIT_NEEDED)
			{
				/*
				 * Sleep(0) indicates that this thread
				 * should be suspended to allow other
				 * waiting threads to execute.
				 */
#ifdef _WIN32
				Sleep(0);
#else // !_WIN32
				usleep(0);
#endif // _WIN32
			}
		}
	}

	return (BC_R_SUCCESS);
}

#ifdef WIN32


// Check directory exits
bool bcDirExists(LPCSTR szDir)
{
	struct __stat64 statbuf;

	if (_stat64(szDir, &statbuf) == 0 && S_ISDIR(statbuf.st_mode))
		return true;
	return false;
}

// Create the directory so that path exists. Returns true if successful or
// if the directory already exists; returns false if unable to create the
// directory for any reason, including if the parent directory does not
// exist. Not named "CreateDirectory" because that's a macro on Windows.
bool bcMakDir(LPCSTR szDir)
{
	int result = _mkdir(szDir);

	if (result == -1)
	{
		return bcDirExists(szDir);  // An error is OK if the directory exists.
	}
	return true;  // No error.
}

uint32_t BCUtf8ToOEM(LPCSTR szUtf8, BCPString &refOEM)
{
	if(szUtf8 == NULL)
		return 0;

	KBPool sPool;
	int wlen = ::MultiByteToWideChar(CP_UTF8, 0, szUtf8, -1, 0, 0);
	LPWSTR wbuf = (LPWSTR)sPool.Calloc(sizeof(wchar_t)*(wlen + 1));
	::MultiByteToWideChar(CP_UTF8, 0, szUtf8, -1, wbuf, wlen);
	wbuf[wlen] = L'0';

	int clen = ::WideCharToMultiByte(CP_OEMCP, 0, wbuf, -1, 0, 0, 0, 0);
	char* cbuf = (char *)sPool.Calloc(sizeof(char)*(clen + 1));
	::WideCharToMultiByte(CP_OEMCP, 0, wbuf, -1, cbuf, clen, 0, 0);
	cbuf[clen] = '\0';

	refOEM = cbuf;

	return refOEM.Len();
}

uint32_t BCOEMToUtf8(LPCSTR szOEM, BCPString &refUtf8)
{
	if(szOEM == NULL)
		return 0;

	KBPool sPool;
	int wlen = ::MultiByteToWideChar(CP_OEMCP, 0, szOEM, -1, 0, 0);
	LPWSTR wbuf = (LPWSTR)sPool.Calloc(sizeof(wchar_t)*(wlen + 1));
	::MultiByteToWideChar(CP_OEMCP, 0, szOEM, -1, wbuf, wlen);
	wbuf[wlen] = L'0';

	int clen = ::WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, 0, 0, 0, 0);
	char* cbuf = (char *)sPool.Calloc(sizeof(char)*(clen + 1));
	::WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, cbuf, clen, 0, 0);
	cbuf[clen] = '\0';

	refUtf8 = cbuf;

	return refUtf8.Len();
}

#else // !WIN32

// Check directory exits
bool bcDirExists(LPCSTR szDir)
{
#if defined(OS_MAC) || defined(OS_IOS)
	struct stat statbuf;
	if (stat(szDir, &statbuf) == 0 && S_ISDIR(statbuf.st_mode))
		return true;
#else // OS_MAC
	struct stat64 statbuf;

	if (stat64(szDir, &statbuf) == 0 && S_ISDIR(statbuf.st_mode))
		return true;
#endif // OS_MAC
	return false;
}

/*
��mode��ʽ��

 S_IRWXU	 					00700Ȩ�ޣ��������ļ�������ӵ�ж���д��ִ�в�����Ȩ��
 S_IRUSR(S_IREAD)	 			00400Ȩ�ޣ��������ļ�������ӵ�пɶ���Ȩ��
 S_IWUSR(S_IWRITE)				00200Ȩ�ޣ��������ļ�������ӵ�п�д��Ȩ��
 S_IXUSR(S_IEXEC)	 			00100Ȩ�ޣ��������ļ�������ӵ��ִ�е�Ȩ��
 S_IRWXG	 					00070Ȩ�ޣ��������ļ��û���ӵ�ж���д��ִ�в�����Ȩ��
 S_IRGRP	 					00040Ȩ�ޣ��������ļ��û���ӵ�пɶ���Ȩ��
 S_IWGRP	 					00020Ȩ�ޣ��������ļ��û���ӵ�п�д��Ȩ��
 S_IXGRP	 					00010Ȩ�ޣ��������ļ��û���ӵ��ִ�е�Ȩ��
 S_IRWXO	 					00007Ȩ�ޣ����������û�ӵ�ж���д��ִ�в�����Ȩ��
 S_IROTH	 					00004Ȩ�ޣ����������û�ӵ�пɶ���Ȩ��
 S_IWOTH	 					00002Ȩ�ޣ����������û�ӵ�п�д��Ȩ��
 S_IXOTH	 					00001Ȩ�ޣ����������û�ӵ��ִ�е�Ȩ��

 */
// Create the directory so that path exists. Returns true if successful or
// if the directory already exists; returns false if unable to create the
// directory for any reason, including if the parent directory does not
// exist. Not named "CreateDirectory" because that's a macro on Windows.
bool bcMakDir(LPCSTR szDir)
{
	int result = mkdir(szDir, S_IRWXU | S_IRWXG | S_IRWXO);

	if (result == -1)
	{
		return bcDirExists(szDir);  // An error is OK if the directory exists.
	}
	return true;  // No error.
}

#endif // WIN32

/* true_random -- generate a crypto-quality random number. */
static int true_random(void)
{
	uint64_t time_now;

	/* crap. this isn't crypto quality, but it will be Good Enough */

	time_now = bc_time_now();
	srand((unsigned int)(((time_now >> 32) ^ time_now) & 0xffffffff));

	return rand() & 0x0FFFF;
}

uint32_t RandomBuffer(void *pBuffer, uint32_t nBufLen)
{
	uint32_t nLenRemain = nBufLen;
	uint32_t nCopyLen = 0;
	uint8_t *pStrBuf = (uint8_t *)pBuffer;
	int32_t nRandom = true_random();
	do
	{
		srand(nRandom);
		nRandom = rand();
		nCopyLen = nLenRemain > 2 ? 2 : nLenRemain;
		memcpy2(pStrBuf, &nRandom, nCopyLen);
		nLenRemain -= nCopyLen;
		pStrBuf += nCopyLen;
	} while (nLenRemain);

	return nBufLen;
}

uint32_t RandomString(void *pBuffer, uint32_t nBufLen)
{
	static const uint8_t szStrTable[62] =
	{
		0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x61, 0x62,
		0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x6B, 0x6C, 0x6D, 0x6E,
		0x6F, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A,
		0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x4B, 0x4C,
		0x4D, 0x4E, 0x4F, 0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
		0x59, 0x5A
	};
	uint8_t *pStrBuf;
	uint32_t nPosition;

	RandomBuffer(pBuffer, nBufLen);
	for (nPosition = 0; nPosition < nBufLen; ++nPosition)
	{
		pStrBuf = (uint8_t *)pBuffer + nPosition;
		*pStrBuf = szStrTable[(*pStrBuf) % 62];
	}

	return nBufLen;
}

BCRESULT DNSGetAddrInfo(
	LPCSTR lpszHost, 
	void *lpBuffer, 
	size_t nBufSize,
	int32_t &refNetType)
{
	struct addrinfo *lpAnswer, *lpCurr;
	int result, n = 0, i = 0;

	if (lpszHost == NULL || strlen(lpszHost) == 0)
	{
		return BC_R_INVALIDPTR;
	}

    result = getaddrinfo(lpszHost, NULL, NULL, &lpAnswer);
	if (result != 0)
	{
		return BC_R_FAILURE;
	}

	for (lpCurr = lpAnswer; lpCurr != NULL; lpCurr = lpCurr->ai_next, n++)
	{
		//
	}
#ifdef _WIN32
	n = rand() % n;
#else // _WIN32
	n = random() % n;
#endif
	for (lpCurr = lpAnswer; lpCurr != NULL; lpCurr = lpCurr->ai_next, i++)
	{
		if (i == n)
		{
            refNetType = lpCurr->ai_family;
            if (!( refNetType == AF_INET || refNetType == AF_INET6)) {
                freeaddrinfo(lpAnswer);
                return BC_R_FAILURE;
            }
            
            if(refNetType == AF_INET6)
			{
				inet_ntop(AF_INET6,
					&(((struct sockaddr_in6 *)(lpCurr->ai_addr))->sin6_addr),
					(char *)lpBuffer, nBufSize);
            }
            else if (refNetType == AF_INET)
			{
				inet_ntop(AF_INET,
					&(((struct sockaddr_in *)(lpCurr->ai_addr))->sin_addr),
					(char *)lpBuffer, nBufSize);
            }
        }
    }
    freeaddrinfo(lpAnswer);
    
	return BC_R_SUCCESS;
}

/**
 * bc_strlcpy:
 * @dest: destination buffer
 * @src: source buffer
 * @dest_size: length of @dest in bytes
 *
 * Portability wrapper that calls strlcpy() on systems which have it,
 * and emulates strlcpy() otherwise. Copies @src to @dest; @dest is
 * guaranteed to be nul-terminated; @src must be nul-terminated;
 * @dest_size is the buffer size, not the number of bytes to copy.
 *
 * At most @dest_size - 1 characters will be copied. Always nul-terminates
 * (unless @dest_size is 0). This function does not allocate memory. Unlike
 * strncpy(), this function doesn't pad @dest (so it's often faster). It
 * returns the size of the attempted result, strlen (src), so if
 * @retval >= @dest_size, truncation occurred.
 *
 * Caveat: strlcpy() is supposedly more secure than strcpy() or strncpy(),
 * but if you really want to avoid screwups, g_strdup() is an even better
 * idea.
 *
 * Returns: length of @src
 */
uint32_t
bc_strlcpy(
	char       *dest,
	const char *src,
	uint32_t        dest_size)
{
	char *d = dest;
	const char *s = src;
	uint32_t n = dest_size;

	if (!dest || !src)
	{
		return 0;
	}

	/* Copy as many bytes as will fit */
	if (n != 0 && --n != 0) {
		do
		{
			char c = *s++;

			*d++ = c;
			if (c == 0)
				break;
		} while (--n != 0);
	}

	/* If not enough room in dest, add NUL and traverse rest of src */
	if (n == 0)
	{
		if (dest_size != 0)
			*d = 0;
		while (*s++)
			;
	}

	return s - src - 1;  /* count does not include NUL */
}

/**
 * bc_strlcat:
 * @dest: destination buffer, already containing one nul-terminated string
 * @src: source buffer
 * @dest_size: length of @dest buffer in bytes (not length of existing string
 *     inside @dest)
 *
 * Portability wrapper that calls strlcat() on systems which have it,
 * and emulates it otherwise. Appends nul-terminated @src string to @dest,
 * guaranteeing nul-termination for @dest. The total size of @dest won't
 * exceed @dest_size.
 *
 * At most @dest_size - 1 characters will be copied. Unlike strncat(),
 * @dest_size is the full size of dest, not the space left over. This
 * function does not allocate memory. It always nul-terminates (unless
 * @dest_size == 0 or there were no nul characters in the @dest_size
 * characters of dest to start with).
 *
 * Caveat: this is supposedly a more secure alternative to strcat() or
 * strncat(), but for real security g_strconcat() is harder to mess up.
 *
 * Returns: size of attempted result, which is MIN (dest_size, strlen
 *     (original dest)) + strlen (src), so if retval >= dest_size,
 *     truncation occurred.
 */
uint32_t
bc_strlcat(char       *dest,
	const char *src,
	uint32_t        dest_size)
{
	char *d = dest;
	const char *s = src;
	uint32_t bytes_left = dest_size;
	uint32_t dlength;  /* Logically, MIN (strlen (d), dest_size) */

	if (!dest || !src)
	{
		return 0;
	}

	/* Find the end of dst and adjust bytes left but don't go past end */
	while (*d != 0 && bytes_left-- != 0)
		d++;
	dlength = d - dest;
	bytes_left = dest_size - dlength;

	if (bytes_left == 0)
		return dlength + strlen(s);

	while (*s != 0)
	{
		if (bytes_left != 1)
		{
			*d++ = *s;
			bytes_left--;
		}
		s++;
	}
	*d = 0;

	return dlength + (s - src);  /* count does not include NUL */
}

int bc_vasprintf(char **str, const char *fmt, va_list args) 
{
	int size = 0;
	va_list tmpa;

	// copy
	va_copy(tmpa, args);

	// apply variadic arguments to
	// sprintf with format to get size
	size = vsnprintf(NULL, size, fmt, tmpa);

	// toss args
	va_end(tmpa);

	// return -1 to be compliant if
	// size is less than 0
	if (size < 0) { return -1; }

	// alloc with size plus 1 for `\0'
	*str = (char *)malloc(size + 1);

	// return -1 to be compliant
	// if pointer is `NULL'
	if (NULL == *str) { return -1; }

	// format string with original
	// variadic arguments and set new size
	size = vsprintf(*str, fmt, args);
	return size;
}

int bc_asprintf(char **str, const char *fmt, ...) 
{
	int size = 0;
	va_list args;

	// init variadic argumens
	va_start(args, fmt);

	// format and get size
	size = bc_vasprintf(str, fmt, args);

	// toss args
	va_end(args);

	return size;
}

void SplitString(
	const ::std::string& str,
	const ::std::string& delimiter,
	::std::vector< ::std::string>* dest)
{
	::std::vector< ::std::string> parsed;
	::std::string::size_type pos = 0;
	while (true)
	{
		const ::std::string::size_type colon = str.find(delimiter, pos);
		if (colon == ::std::string::npos)
		{
			parsed.push_back(str.substr(pos));
			break;
		}
		else
		{
			parsed.push_back(str.substr(pos, colon - pos));
			pos = colon + 1;
		}
	}
	dest->swap(parsed);
}

void SplitStringByFirstDelimiter(
	const ::std::string& str,
	const ::std::string& delimiter,
	::std::vector< ::std::string>* dest)
{
	::std::vector< ::std::string> parsed;
	::std::string::size_type pos = 0;
	const ::std::string::size_type colon = str.find(delimiter, pos);
	if (colon == ::std::string::npos)
	{
		parsed.push_back(str.substr(pos));
	}
	else
	{
		parsed.push_back(str.substr(pos, colon - pos));
		pos = colon + 1;
		parsed.push_back(str.substr(pos));
	}
	dest->swap(parsed);
}

bool IsPathAbsolute(const std::string& path) 
{
#if defined(OS_WIN)
	if (path.length() >= 2 && path[1] == ':' &&
		((path[0] >= 'A' && path[0] <= 'Z') ||
			(path[0] >= 'a' && path[0] <= 'z'))) {
		return path.length() > 2 && (path[2] == '\\' || path[2] == '/');
	}
	// Look for a pair of leading separators.
	return path.length() > 1 && path[0] == '\\' && path[1] == '\\';
#else  // OS_WIN
	// Look for a separator in the first position.
	return path.length() > 0 && path[0] == '/';
#endif  // OS_WIN
}

std::string GetBasePathName(const std::string& path) 
{
	std::string strFile(path);
	BCPString strWhere, strWhat;
	for (auto &it : strFile)
	{
		if (it == '\\')
		{
			it = '/';
		}
	}
	return strFile.substr(strFile.rfind('/') + 1);
}


#if _MSC_VER
#define snprintf _snprintf
#endif

#define STACK_INFO_LEN  1024

#ifdef OS_WIN
void ShowTraceStack(const char* szBriefInfo)
{
    static const int MAX_STACK_FRAMES = 12;
    void* pStack[MAX_STACK_FRAMES];
    //char *szStackInfo = (char *)_malloca(STACK_INFO_LEN * MAX_STACK_FRAMES);
    //char *szFrameInfo = (char *)_malloca(STACK_INFO_LEN);
    char szStackInfo[STACK_INFO_LEN * MAX_STACK_FRAMES];
    char szFrameInfo[STACK_INFO_LEN];

    HANDLE process = GetCurrentProcess();
	if (process == INVALID_HANDLE_VALUE)
	{
		//DWORD dwCode = GetLastError();
		//LogError(_LOCAL_, "%Failed to call GetCurrentProcess[error code:%d]", dwCode);
		//return;
	}
    SymInitialize(process, NULL, TRUE);
    WORD frames = CaptureStackBackTrace(0, MAX_STACK_FRAMES, pStack, NULL);
    snprintf(szStackInfo, sizeof(szStackInfo), "[%s]:\n", szBriefInfo == NULL ? 
		"stack traceback" : szBriefInfo);

    for (WORD i = 0; i < frames; ++i) {
        DWORD64 address = (DWORD64)(pStack[i]);

        DWORD64 displacementSym = 0;
        char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(TCHAR)];
        PSYMBOL_INFO pSymbol = (PSYMBOL_INFO)buffer;
        pSymbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        pSymbol->MaxNameLen = MAX_SYM_NAME;

        DWORD displacementLine = 0;
        IMAGEHLP_LINE64 line;
        line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);

        if (SymFromAddr(process, address, &displacementSym, pSymbol) &&
            SymGetLineFromAddr64(process, address, &displacementLine, &line))
        {
            snprintf(szFrameInfo, sizeof(szFrameInfo), "\t%s() at %s:%d(0x%llx)\n",
                pSymbol->Name, line.FileName, line.LineNumber, pSymbol->Address);
        }
        else
        {
            snprintf(szFrameInfo, sizeof(szFrameInfo), "\terror: %d\n", GetLastError());
        }
        strcat(szStackInfo, szFrameInfo);
    }

    LogDebug(_LOCAL_, "%s", szStackInfo); 
}
#elif defined(OS_ADROID)
void ShowTraceStack(const char* szBriefInfo)
{
    void* array[20];
    int    size;
    char** strings;
    int    i;

    size = backtrace(array, 20);

    strings = backtrace_symbols(array, size);

    LogDebug(_LOCAL_, "[%s]Obtained %d stack frames:\n", szBriefInfo? szBriefInfo:"", size);

    for (i = 0; i < size; i++) {
        LogDebug(_LOCAL_, "%s", strings[i]);
    }

    free(strings);
}
#endif // OS_WIN

///////////////////////////////////////////////////////////////////////////////
// Namespace: BC
///////////////////////////////////////////////////////////////////////////////

} // End of namespace BC

///////////////////////////////////////////////////////////////////////////////
// End of file...
///////////////////////////////////////////////////////////////////////////////
