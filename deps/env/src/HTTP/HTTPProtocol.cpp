///////////////////////////////////////////////////////////////////////////////
// file : HTTPProtocol.cpp
// author : antoniozhou
///////////////////////////////////////////////////////////////////////////////

#include <BC/BCAscii.h>
#include "StringUtils.h"
#include "HTTPProtocol.h"


///////////////////////////////////////////////////////////////////////////////
// Namespace : HTTP
///////////////////////////////////////////////////////////////////////////////

namespace HTTP
{

#ifdef DEBUG_HTTPD
#define ENTER(x)	do { fprintf(stderr, "ENTER %s\n", (x)); } while (0)
#define EXIT(x)		do { fprintf(stderr, "EXIT %s\n", (x)); } while (0)
#define NOTICE(x)	do { fprintf(stderr, "NOTICE %s\n", (x)); } while (0)
#else
#define ENTER(x)	do { } while(0)
#define EXIT(x)		do { } while(0)
#define NOTICE(x)	do { } while(0)
#endif

const char G_szHTTP10[] = "HTTP/1.0";
const char G_szHTTP11[] = "HTTP/1.1";

#define LENGTHOK(s) ((s) - szBuf < (int)nBufLen)
#define BUFLENOK(s) ((s) - szBuf < HTTP_RECVLEN)

static LPCVOID bcMemFind(LPCVOID lpSrc, size_t nSrcSize, LPCVOID lpDest, size_t nDestSize)
{
	if (lpSrc == NULL || lpDest == NULL || nSrcSize < nDestSize)
	{
		return NULL;
	}
	LPCSTR lpChSrc = (LPCSTR)lpSrc;
	size_t n = nSrcSize - nDestSize;
	for (size_t i = 0;i <= n;i++)
	{
		if (memcmp(lpChSrc + i, lpDest, nDestSize) == 0)
		{
			return lpChSrc + i;
		}
	}
	return NULL;
}

static LPCSTR FindEndMask(LPCSTR lpStart, size_t nSize, size_t &refSizeEnd)
{
	LPCSTR lpStandard = NULL, lpNewLine = NULL, lpReturn = NULL;
	LPCSTR lpResult = NULL;

	lpStandard = (LPCSTR)bcMemFind(lpStart, nSize, "\r\n", 2);
	lpNewLine = (LPCSTR)bcMemFind(lpStart, nSize, "\n", 1);
	lpReturn = (LPCSTR)bcMemFind(lpStart, nSize, "\r", 1);
	lpResult = BCMIN(lpStandard, lpNewLine);
	if (lpStandard && lpNewLine && lpReturn)
	{
		lpResult = BCMIN(lpStandard, lpNewLine);
		lpResult = BCMIN(lpResult, lpReturn);
	}
	else if (lpStandard && lpNewLine)
	{
		lpResult = BCMIN(lpStandard, lpNewLine);
	}
	else if (lpStandard && lpReturn)
	{
		lpResult = BCMIN(lpStandard, lpReturn);
	}
	else if (lpNewLine && lpReturn)
	{
		lpResult = BCMIN(lpNewLine, lpReturn);
	}
	else if (lpStandard)
	{
		lpResult = lpStandard;
	}
	else if (lpNewLine)
	{
		lpResult = lpNewLine;
	}
	else if (lpReturn)
	{
		lpResult = lpReturn;
	}
	else
	{
		lpResult = NULL;
	}
	if (lpResult)
	{
		if (lpResult == lpStandard)
		{
			refSizeEnd = 2;
		}
		else
		{
			refSizeEnd = 1;
		}
	}
	return lpResult;
}

static BOOL IsValidPair(
	LPCSTR lpStart, 
	size_t nSize, 
	LPCSTR &lpValue, 
	size_t &nKeyLen, 
	size_t &nValLen)
{
	LPCSTR lpSplitter, lpSrc;

	if (lpStart == NULL || nSize <= 1)
	{
		return FALSE;
	}	

	lpSrc = (LPCSTR)lpStart;
	lpSplitter = (LPCSTR)bcMemFind(lpSrc, nSize, ":", 1);
	if (lpSplitter != NULL)
	{
		nKeyLen = lpSplitter - lpSrc;
		lpValue = lpSplitter + 1;
		nValLen = lpSrc + nSize - lpValue;
		return TRUE;
	}
	return FALSE;
}

static BCRESULT FindPair(
	LPCSTR lpStart, 
	size_t nSize, 
	BCStrPtrLen &refKey, 
	BCStrPtrLen &refValue, 
	size_t &refSizePair)
{
	LPCSTR lpDst = NULL, lpValue = NULL;
	size_t nKeySize = 0, nValSize = 0, nSizeEnd = 0;

	lpDst = FindEndMask(lpStart, nSize, nSizeEnd);
	if (lpDst == NULL)
	{
		lpDst = lpStart + nSize;
		nSizeEnd = 0;
	}
	if (IsValidPair(lpStart, lpDst - lpStart, lpValue, nKeySize, nValSize))
	{
		BCStrPtrLen strKey((char *)lpStart, nKeySize);
		BCStrPtrLen strValue((char *)lpValue, nValSize);
		StringParser sKeyParser(&strKey);
		StringParser sValParser(&strValue);

		sKeyParser.ConsumeWhitespace();
		sKeyParser.ConsumeUntilWhitespace(&refKey);
		if (refKey.Ptr == NULL || refKey.Len == 0)
		{
			return BC_R_INVALIDPTR;
		}
		sValParser.ConsumeWhitespace();
		sValParser.ConsumeLength(&refValue, nValSize - sValParser.GetCurrentLineNumber());
/*		if (refValue.Ptr == NULL || refValue.Len == 0)
		{
			return BC_R_INVALIDPTR;
		}
*/
		refSizePair = lpDst - lpStart + nSizeEnd;
		return BC_R_SUCCESS;
	}
	return BC_R_INVALIDPTR;	
}


///////////////////////////////////////////////////////////////////////////////
// class : HTTPReq
///////////////////////////////////////////////////////////////////////////////

HTTPReq::HTTPReq()
	: m_eIoState(BC_HTTPD_STATEIDLE)
	, m_bRequest(false)
	, m_eMethod(0)
	, m_szUrl(NULL)
	, m_szHost(NULL)
	, m_szProtocol(NULL)
	, m_nStatusCode(0)
	, m_szStatusCode(NULL)
	, m_nFlags(0)
	, m_szMimeType(NULL)
	, m_nRetCode(0)
	, m_szRetMsg(NULL)
{
	//
}

HTTPReq::~HTTPReq()
{
	//
}

BCRESULT HTTPReq::ParseResponse(BCBuffer &refBuffer)
{
	char *s, *szBuf;
	char *p, *szValue = NULL, *lpEndMask = NULL;
	uint32_t delim;
	size_t nSkip = 0, nBufLen = 0;
	BCStrPtrLen strKey, strValue;
	BCRESULT result;

	ENTER("response");

	szBuf = (char *)refBuffer.Current();
	nBufLen = refBuffer.RemainingLength();
	/*
	 * If we don't find a blank line in our buffer, return that we need
	 * more data.
	 */
	s = (char *)bcMemFind(szBuf, nBufLen, "\r\n\r\n", 4);
	delim = 2;
	if (s == NULL)
	{
		s = (char *)bcMemFind(szBuf, nBufLen, "\n\n", 2);
		delim = 1;
	}
	if (s == NULL)
		return (BC_R_NOTFOUND);

	nBufLen = s - szBuf;
	// Parse protocol
	p = szBuf;
	if (strncmp(p, G_szHTTP11, 8) == 0)
	{
		m_szProtocol = G_szHTTP11;
	}
	else if (strncmp(p, G_szHTTP10, 8) == 0)
	{
		m_szProtocol = G_szHTTP10;
	}
	else
	{
		return BC_R_UNEXPECTED;
	}
	// Parse status code
	p += 9;
	szValue = p;
	p[3] = 0;
	m_nStatusCode = atoi(szValue);
	szValue += 4;
	// Parse status text
	lpEndMask = (char *)FindEndMask(szValue, s - szValue, nSkip);
	if (lpEndMask == NULL)
	{
		return BC_R_INVALIDPTR;
	}
	m_szStatusCode = m_sPool.Strndup(szValue, lpEndMask - szValue);

	// Parse header
	p = lpEndMask + nSkip;
	while(p < s)
	{
		result = FindPair(p, s - p, strKey, strValue, nSkip);
		if (result != BC_R_SUCCESS)
		{
			goto return_error;
		}		
		else
		{
			KVPairS sPair;

			sPair.szArg = m_sPool.Strndup(strKey.Ptr, strKey.Len);
			sPair.szValue = m_sPool.Strndup(strValue.Ptr, strValue.Len);
			m_arrayHeaders.push_back(sPair);

		}
		p += nSkip;
	}

	/*
	 * From now on, p is the start of our buffer.
	 */
	refBuffer.Forward(nBufLen + delim*2);

	EXIT("response");

	return (BC_R_SUCCESS);

return_error:
	return result;
}

BCRESULT HTTPReq::Response()
{
	uint32_t needlen;
	char strBuf[1024];

	needlen = strlen(m_szProtocol) + 1; /* protocol + space */
	needlen += 3 + 1;  /* room for response code, always 3 bytes */
	needlen += strlen(m_szRetMsg) + 2;  /* return msg + CRLF */

	snprintf(strBuf, 1024, "%s %03d %s\r\n",
		m_szProtocol, m_nRetCode, m_szRetMsg);
	m_sHeaderBuffer.Write(strBuf, needlen);

	return (BC_R_SUCCESS);
}

BCRESULT HTTPReq::CreateRequest(uint32_t eMethod, LPCSTR szUrl, LPCSTR szProtocol /* = G_szHTTP11 */)
{
	ASSERT(szUrl != NULL);
	ASSERT(szProtocol != NULL);

	m_bRequest = true;

	m_eMethod = eMethod;
	m_szUrl = m_sPool.Strdup(szUrl);
	m_szProtocol = m_sPool.Strdup(szProtocol);

	return BC_R_SUCCESS;
}

BCRESULT HTTPReq::BeginHeader(BOOL bEncodeUrl, BOOL bPCS)
{
	if (!m_bRequest)
	{
		return BC_R_SUCCESS;
	}
	else
	{
		BCBOStream sWriter(&m_sHeaderBuffer);
		switch(m_eMethod)
		{
		case BC_HTTPD_METHODGET:
			sWriter.WriteStringExact("GET /");
			break;
		case BC_HTTPD_METHODPOST:
			sWriter.WriteStringExact("POST /");
			break;
		case BC_HTTPD_METHODPUT:
			sWriter.WriteStringExact("PUT /");
			break;
		default:
			return BC_R_NOTIMPLEMENTED;
		}
		if (m_szUrl)
		{
			if (strncasecmp(m_szUrl, "http://", 7) == 0)
			{
				BCStrPtrLen strHost, strPort, strRelative;
				bool bSSL = false;
				uint16_t nPort = 80;

				if (BC_R_SUCCESS == HTTPProtocol::ParseHost(m_szUrl,
					strHost, strPort, strRelative, bSSL, nPort))
				{
					char szHost[256];
					
					szHost[0] = 0;
					if (strHost.Len > 2048 || strPort.Len > 6 || strRelative.Len > 2048)
					{
						return BC_R_INVALIDPTR;
					}
					strncat(szHost, strHost.Ptr, strHost.Len);
					if (strPort.Ptr != NULL && strPort.Len != 0)
					{
						strcat(szHost, ":");
						strncat(szHost, strPort.Ptr, strPort.Len);
					}
					m_szHost = m_sPool.Strdup(szHost);
					if (bEncodeUrl)
					{
						KBPool sPool;
						char *lpBuffer;
						size_t nBufSize = 0;

						nBufSize = strRelative.Len * 3;
						lpBuffer = (char *)sPool.Calloc(nBufSize);
						if (lpBuffer == NULL)
						{
							return BC_R_NOMEMORY;
						}

						if(bPCS)
						{
							char *lpBufferPCSFileName, *lpBufferPCSFront;
							size_t nBufSizePCSFileName = 0, nBufSizePCSFront = 0;

							nBufSizePCSFileName = strRelative.Len * 3;
							nBufSizePCSFront = nBufSizePCSFileName;
							lpBufferPCSFileName = (char*)sPool.Calloc(nBufSizePCSFileName);
							lpBufferPCSFront = (char*)sPool.Calloc(nBufSizePCSFileName);
							if(lpBufferPCSFileName == NULL)
								return BC_R_NOMEMORY;

//							const char* split = strstr(strRelative.Ptr, "&path=");
							const char* split = strstr(strRelative.Ptr, "&to=");
							ASSERT(split);
							snprintf(lpBufferPCSFront, strRelative.Len - strlen(split) + 5, "%s", strRelative.Ptr);
							nBufSizePCSFileName = HTTPProtocol::URLEncode(split + 4, lpBufferPCSFileName, nBufSizePCSFileName);
							snprintf(lpBuffer, nBufSizePCSFront, "%s%s", lpBufferPCSFront, lpBufferPCSFileName); sWriter.Write(lpBuffer, strlen(lpBuffer));
						}
						else
						{
							sWriter.Write(strRelative.Ptr, strRelative.Len);
						}
					}
					else
					{
						sWriter.Write(strRelative.Ptr, strRelative.Len);
					}
				}
				else
				{
					sWriter.WriteStringExact(m_szUrl);
				}
			}
			else
			{
				sWriter.WriteStringExact(m_szUrl);
			}
		}
		else
		{
			sWriter.WriteStringExact("/");
		}
	}
	return BC_R_SUCCESS;
}

BCRESULT HTTPReq::AddParam(const char *name, const char *val)
{
	KVPairS sPair;

	sPair.szArg = m_sPool.Strdup(name);
	sPair.szValue = m_sPool.Strdup(val);
	m_arrayArgs.push_back(sPair);

	return (BC_R_SUCCESS);
}

BCRESULT HTTPReq::AddParamUInt(const char *name, int val)
{
	char buf[sizeof "18446744073709551616"];

	sprintf(buf, "%d", val);

	return AddParam(name, buf);
}

BCRESULT HTTPReq::AddParamInt64(const char *name, int64_t val)
{
	char buf[64];

	sprintf(buf, "%" _S64BITARG_, val);

	return AddParam(name, buf);
}

BCRESULT HTTPReq::EndParams(BOOL host /* = TRUE */)
{
	ArgArray::iterator pIter, pIterEnd;
	BCBOStream sWriter(&m_sHeaderBuffer);
	bool bFirst = true;

	pIter = m_arrayArgs.begin();
	pIterEnd = m_arrayArgs.end();
	for (;pIter != pIterEnd;pIter++)
	{
		if (bFirst)
		{
			sWriter.WriteStringExact("?");
			bFirst = false;
		}
		else
		{
			sWriter.WriteStringExact("&");
		}
		sWriter.WriteStringExact((*pIter).szArg);
		sWriter.WriteStringExact("=");
		sWriter.WriteStringExact((*pIter).szValue);
	}
	sWriter.WriteStringExact(" ");
	sWriter.WriteStringExact(m_szProtocol);
	sWriter.WriteStringExact("\r\n");
	if (m_szHost && host)
	{
		AddHeader("Host", m_szHost);
	}

	return BC_R_SUCCESS;
}

BCRESULT HTTPReq::AddHeader(const char *name, const char *val)
{
	uint32_t needlen;
	char strBuf[1024];

	needlen = strlen(name); /* name itself */
	if (val != NULL)
		needlen += 2 + strlen(val); /* :<space> and val */
	needlen += 2; /* CRLF */

	if (val != NULL)
	{
		snprintf(strBuf, 1024,	"%s: %s\r\n", name, val);
	}
	else
	{
		snprintf(strBuf, 1024, "%s\r\n", name);
	}

	m_sHeaderBuffer.Write(strBuf, needlen);

	return (BC_R_SUCCESS);
}

BCRESULT HTTPReq::AddHeaderUInt(const char *name, int val)
{
	uint32_t needlen;
	char buf[sizeof "18446744073709551616"];
	char strBuf[1024];

	sprintf(buf, "%d", val);

	needlen = strlen(name); /* name itself */
	needlen += 2 + strlen(buf); /* :<space> and val */
	needlen += 2; /* CRLF */

	snprintf(strBuf, 1024, "%s: %s\r\n", name, buf);

	m_sHeaderBuffer.Write(strBuf, needlen);

	return (BC_R_SUCCESS);
}

BCRESULT HTTPReq::AddHeaderInt64(const char *name, int64_t val)
{
	uint32_t needlen;
	char buf[64];
	char strBuf[1024];

	sprintf(buf, "%" _S64BITARG_, val);

	needlen = strlen(name); /* name itself */
	needlen += 2 + strlen(buf); /* :<space> and val */
	needlen += 2; /* CRLF */

	snprintf(strBuf, 1024, "%s: %s\r\n", name, buf);

	m_sHeaderBuffer.Write(strBuf, needlen);

	return (BC_R_SUCCESS);
}

BCRESULT HTTPReq::EndHeaders()
{
	m_sHeaderBuffer.Write("\r\n", 2);

	return (BC_R_SUCCESS);
}

void HTTPReq::Reset()
{
	/*
	 * Catch errors here.  We MUST be in RECV mode, and we MUST NOT have
	 * any outstanding buffers.  If we have buffers, we have a leak.
	 */
	ASSERT(BC_HTTPD_ISRECV(this));

	m_eMethod = BC_HTTPD_METHODUNKNOWN;
	m_szUrl = NULL;
	m_szProtocol = NULL;
	m_nFlags = 0;

	m_sHeaderBuffer.Reset(1);
	m_sBodyBuffer.Reset(1);
}

BCRESULT HTTPReq::Render(HTTPStatusCode eStatus)
{
	BCStrPtrLen *pStrStatus;

	pStrStatus = HTTPProtocol::GetStatusCodeString(eStatus);
	m_nRetCode = HTTPProtocol::GetStatusCode(eStatus);
	m_szRetMsg = pStrStatus->Ptr;
	m_szMimeType = "text/plain";
	m_sBodyBuffer.Reset(1);
	m_sBodyBuffer.Write(pStrStatus->Ptr, pStrStatus->Len);

	return (BC_R_SUCCESS);
}

BCRESULT HTTPReq::Render404()
{
	return Render(httpNotFound);
}

BCRESULT HTTPReq::Render500()
{
	return Render(httpInternalServerError);
}

LPCSTR HTTPReq::GetParam(LPCSTR szArg, BOOL bCaseSensitive /* = FALSE */)
{
	KVPairS *pPair;

	ASSERT(szArg);
	if (bCaseSensitive)
	{
		for (uint32_t i = 0;i < m_arrayArgs.size();i++)
		{
			pPair = &m_arrayArgs[i];
			ASSERT(pPair->szArg);
			if (strcmp(pPair->szArg, szArg) == 0)
			{
				return pPair->szValue;
			}
		}
	}
	else
	{
		for (uint32_t i = 0;i < m_arrayArgs.size();i++)
		{
			pPair = &m_arrayArgs[i];
			ASSERT(pPair->szArg);
			if (strcasecmp(pPair->szArg, szArg) == 0)
			{
				return pPair->szValue;
			}
		}
	}
	return NULL;
}

LPCSTR HTTPReq::GetHeader(LPCSTR szArg, BOOL bCaseSensitive /* = FALSE */)
{
	KVPairS *pPair;

	ASSERT(szArg);
	if (bCaseSensitive)
	{
		for (uint32_t i = 0;i < m_arrayHeaders.size();i++)
		{
			pPair = &m_arrayHeaders[i];
			ASSERT(pPair->szArg);
			if (strcmp(pPair->szArg, szArg) == 0)
			{
				return pPair->szValue;
			}
		}
	}
	else
	{
		for (uint32_t i = 0;i < m_arrayHeaders.size();i++)
		{
			pPair = &m_arrayHeaders[i];
			ASSERT(pPair->szArg);
			if (strcasecmp(pPair->szArg, szArg) == 0)
			{
				return pPair->szValue;
			}
		}
	}
	return NULL;
}

uint64_t HTTPReq::GetContentLength()
{
	LPCSTR lpCL;

	lpCL = GetHeader("Content-Length");
	if (lpCL != NULL)
	{
		return atoll(lpCL);
	}
	return 0;
}

void HTTPReq::GetMD5(char* md5)
{
	LPCSTR lpCL;
	lpCL = GetHeader("Content-MD5");
	if(lpCL != NULL)
		sprintf(md5, "%s", lpCL);
}

void HTTPReq::GetETag(char* etag)
{
	LPCSTR lpCL;
	lpCL = GetHeader("ETag");
	if(lpCL != NULL)
		sprintf(etag, "%s", lpCL);
}

bool HTTPReq::IsChunkedBody()
{
	LPCSTR lpszTE;

	lpszTE = GetHeader("Transfer-Encoding");
	if (lpszTE != NULL)
	{
		return strncasecmp(lpszTE, "chunked", 7) == 0;
	}
	return false;
}

///////////////////////////////////////////////////////////////////////////////
// class : HTTPResp
///////////////////////////////////////////////////////////////////////////////

HTTPResp::HTTPResp()
	: m_eIoState(BC_HTTPD_STATEIDLE)
	, m_eMethod(0)
	, m_szUrl(NULL)
	, m_szProtocol(NULL)
	, m_nFlags(0)
	, m_szMimeType(NULL)
	, m_nRetCode(0)
	, m_szRetMsg(NULL)
{
	m_szProtocol = (char *)G_szHTTP11;
	memzero(&m_sRemoteAddr, sizeof(m_sRemoteAddr));
	memzero(&m_sLocalAddr, sizeof(m_sLocalAddr));
}

HTTPResp::~HTTPResp()
{
	//
}

BCRESULT HTTPResp::ParseRequest(BCBuffer &refBuffer)
{
	char *s, *szBuf, *szHeader;
	char *p, *szNext, *szArg, *szValue;
	int delim;
	KVPairS sPair;
	uint32_t nBufLen = 0;

	ENTER("request");

	szBuf = (char *)refBuffer.Current();
	/*
	 * If we don't find a blank line in our buffer, return that we need
	 * more data.
	 */
	s = strstr(szBuf, "\r\n\r\n");
	delim = 2;
	if (s == NULL)
	{
		s = strstr(szBuf, "\n\n");
		delim = 1;
	}
	if (s == NULL)
		return (BC_R_NOTFOUND);

	nBufLen = s - szBuf;

	/*
	 * Determine if this is a POST or GET method.  Any other values will
	 * cause an error to be returned.
	 */
	if (strncmp(szBuf, "GET ", 4) == 0)
	{
		m_eMethod = BC_HTTPD_METHODGET;
		p = szBuf + 4;
	}
	else if (strncmp(szBuf, "POST ", 5) == 0)
	{
		m_eMethod = BC_HTTPD_METHODPOST;
		p = szBuf + 5;
	}
	else
	{
		return (BC_R_RANGE);
	}

	/*
	 * From now on, p is the start of our buffer.
	 */

	/*
	 * Extract the URL.
	 */
	s = p;
	while (LENGTHOK(s) && BUFLENOK(s) &&
	       (*s != '\n' && *s != '\r' && *s != '\0' && *s != ' '))
		s++;
	if (!LENGTHOK(s))
		return (BC_R_NOTFOUND);
	if (!BUFLENOK(s))
		return (BC_R_NOMEMORY);
	*s = 0;

	/*
	 * Make the URL relative.
	 */
	if ((strncmp(p, "http:/", 6) == 0) || (strncmp(p, "https:/", 7) == 0))
	{
		/* Skip first / */
		while (*p != '/' && *p != 0)
			p++;
		if (*p == 0)
			return (BC_R_RANGE);
		p++;
		/* Skip second / */
		while (*p != '/' && *p != 0)
			p++;
		if (*p == 0)
			return (BC_R_RANGE);
		p++;
		/* Find third / */
		while (*p != '/' && *p != 0)
			p++;
		if (*p == 0)
		{
			p--;
			*p = '/';
		}
	}

	m_szUrl = p;
	// Skip whitespace
	p = s + (delim==2?1:2);
	s = p;

	/*
	 * Now, see if there is a ? mark in the URL.  If so, this is
	 * part of the query string, and we will split it from the URL.
	 */
	p = strchr(m_szUrl, '?');
	if (p)
	{
		*p = 0; // Set end of page string
		while(p && p < s)
		{
			p++; // skip ? or &
			szNext = strstr(p, "&");
			if (szNext == NULL)
			{
				szNext = s;
			}
			szArg = p;
			szValue = strstr(p, "=");
			if (szValue && szValue < szNext)
			{
				sPair.szArg = m_sPool.Strndup(szArg, szValue - szArg);
				szValue++;
				sPair.szValue = m_sPool.Strndup(szValue, szNext - szValue);
			}
			else
			{
				sPair.szArg = m_sPool.Strndup(szArg, szValue - szArg);
				sPair.szValue = NULL;
			}
			m_arrayArgs.push_back(sPair);
			p = szNext;
		}
	}
	m_szUrl = m_sPool.Strdup(m_szUrl);
	p = s;

	/*
	 * Extract the HTTP/1.X protocol.  We will bounce on anything but
	 * HTTP/1.1 for now.
	 */
	while (LENGTHOK(s) && BUFLENOK(s) && (*s != '\n' && *s != '\r' && *s != '\0'))
		s++;
	if (!LENGTHOK(s))
		return (BC_R_NOTFOUND);
	if (!BUFLENOK(s))
		return (BC_R_NOMEMORY);
	*s = 0;
	if ((strncmp(p, "HTTP/1.0", 8) != 0) && (strncmp(p, "HTTP/1.1", 8) != 0))
		return (BC_R_RANGE);
	m_szProtocol = m_sPool.Strdup(p);

	// Parse header
	p = s + delim;
	s = szBuf + nBufLen;
	while(p < s)
	{
		szNext = NULL;
		if (delim == 2)
		{
			szNext = strstr(p, "\r\n");
		}
		else
		{
			szNext = strstr(p, "\n");
		}
		if (szNext && szNext <= s)
		{
			KVPairS sPair;
			szHeader = p;
			szValue = strstr(p, ": ");
			if (szValue)
			{
				sPair.szArg = m_sPool.Strndup(szHeader, szValue - szHeader);
				szValue += 2;
				sPair.szValue = m_sPool.Strndup(szValue, szNext - szValue);
				m_arrayHeaders.push_back(sPair);
			}
			else
			{
				return BC_R_UNEXPECTED;
			}
		}
		p = szNext + delim;
	}

	if ((szArg = (LPSTR)GetHeader("Connection")) != NULL &&
		strcasecmp(szArg, "close") == 0)
		m_nFlags |= HTTPD_CLOSE;

	if (GetHeader("Host") != NULL)
		m_nFlags |= HTTPD_FOUNDHOST;

	/*
	 * Standards compliance hooks here.
	 */
	if (strcmp(m_szProtocol, "HTTP/1.1") == 0
	    && ((m_nFlags & HTTPD_FOUNDHOST) == 0))
		return (BC_R_RANGE);

	refBuffer.Forward(nBufLen + delim*2);

	return (BC_R_SUCCESS);
}

BCRESULT HTTPResp::Response()
{
	uint32_t needlen;
	char strBuf[1024];

	needlen = strlen(m_szProtocol) + 1; /* protocol + space */
	needlen += 3 + 1;  /* room for response code, always 3 bytes */
	needlen += strlen(m_szRetMsg) + 2;  /* return msg + CRLF */

	snprintf(strBuf, 1024, "%s %03d %s\r\n",
		m_szProtocol, m_nRetCode, m_szRetMsg);
	m_sHeaderBuffer.Write(strBuf, needlen);

	return (BC_R_SUCCESS);
}

BCRESULT HTTPResp::AddHeader(const char *name, const char *val)
{
	uint32_t needlen;
	char strBuf[1024];

	needlen = strlen(name); /* name itself */
	if (val != NULL)
		needlen += 2 + strlen(val); /* :<space> and val */
	needlen += 2; /* CRLF */

	if (val != NULL)
	{
		snprintf(strBuf, 1024,	"%s: %s\r\n", name, val);
	}
	else
	{
		snprintf(strBuf, 1024, "%s\r\n", name);
	}

	m_sHeaderBuffer.Write(strBuf, needlen);

	return (BC_R_SUCCESS);
}

BCRESULT HTTPResp::AddHeaderBool(const char *name, bool val)
{
	return AddHeader(name, val ? "true" : "false");
}

BCRESULT HTTPResp::AddHeaderUInt(const char *name, int val)
{
	uint32_t needlen;
	char buf[sizeof "18446744073709551616"];
	char strBuf[1024];

	sprintf(buf, "%d", val);

	needlen = strlen(name); /* name itself */
	needlen += 2 + strlen(buf); /* :<space> and val */
	needlen += 2; /* CRLF */

	snprintf(strBuf, 1024, "%s: %s\r\n", name, buf);

	m_sHeaderBuffer.Write(strBuf, needlen);

	return (BC_R_SUCCESS);
}

BCRESULT HTTPResp::AddHeaderInt64(const char *name, int64_t val)
{
	uint32_t needlen;
	char buf[128];
	char strBuf[1024];

	sprintf(buf, "%" _S64BITARG_, val);

	needlen = strlen(name); /* name itself */
	needlen += 2 + strlen(buf); /* :<space> and val */
	needlen += 2; /* CRLF */

	snprintf(strBuf, 1024, "%s: %s\r\n", name, buf);

	m_sHeaderBuffer.Write(strBuf, needlen);

	return (BC_R_SUCCESS);
}

BCRESULT HTTPResp::EndHeaders()
{
	m_sHeaderBuffer.Write("\r\n", 2);

	return (BC_R_SUCCESS);
}

void HTTPResp::Reset()
{
	/*
	 * Catch errors here.  We MUST be in RECV mode, and we MUST NOT have
	 * any outstanding buffers.  If we have buffers, we have a leak.
	 */
	ASSERT(BC_HTTPD_ISRECV(this));

	m_eMethod = BC_HTTPD_METHODUNKNOWN;
	m_szUrl = NULL;
	m_szProtocol = NULL;
	m_nFlags = 0;

	m_sHeaderBuffer.Reset(0);
	m_sBodyBuffer.Reset(0);
	m_sPool.Clear();
	m_arrayArgs.clear();
	m_arrayHeaders.clear();
}

BCRESULT HTTPResp::Render(HTTPStatusCode eStatus, BOOL bWriteRetMsg /*= TRUE*/)
{
	if (bWriteRetMsg)
	{
		BCStrPtrLen *pStrStatus;

		pStrStatus = HTTPProtocol::GetStatusCodeString(eStatus);
		m_szRetMsg = pStrStatus->Ptr;
	}
	m_nRetCode = HTTPProtocol::GetStatusCode(eStatus);
	m_szMimeType = "text/plain";
	m_sBodyBuffer.Reset(0);

	return (BC_R_SUCCESS);
}

BCRESULT HTTPResp::Render404()
{
	return Render(httpNotFound);
}

BCRESULT HTTPResp::Render500()
{
	return Render(httpInternalServerError);
}

LPCSTR HTTPResp::GetParam(LPCSTR szArg, BOOL bCaseSensitive /* = FALSE */)
{
	KVPairS *pPair;

	ASSERT(szArg);

	if (bCaseSensitive)
	{
		for (uint32_t i = 0;i < m_arrayArgs.size();i++)
		{
			pPair = &m_arrayArgs[i];
			ASSERT(pPair->szArg);
			if (strcmp(pPair->szArg, szArg) == 0)
			{
				return pPair->szValue;
			}
		}
	}
	else
	{
		for (uint32_t i = 0;i < m_arrayArgs.size();i++)
		{
			pPair = &m_arrayArgs[i];
			ASSERT(pPair->szArg);
			if (strcasecmp(pPair->szArg, szArg) == 0)
			{
				return pPair->szValue;
			}
		}
	}
	return NULL;
}

LPCSTR HTTPResp::GetHeader(LPCSTR szArg, BOOL bCaseSensitive /* = FALSE */)
{
	KVPairS *pPair;

	ASSERT(szArg);

	if (bCaseSensitive)
	{
		for (uint32_t i = 0;i < m_arrayHeaders.size();i++)
		{
			pPair = &m_arrayHeaders[i];
			ASSERT(pPair->szArg);
			if (strcmp(pPair->szArg, szArg) == 0)
			{
				return pPair->szValue;
			}
		}
	}
	else
	{
		for (uint32_t i = 0;i < m_arrayHeaders.size();i++)
		{
			pPair = &m_arrayHeaders[i];
			ASSERT(pPair->szArg);
			if (strcasecmp(pPair->szArg, szArg) == 0)
			{
				return pPair->szValue;
			}
		}
	}
	return NULL;
}

HTTPResp::RangeS HTTPResp::GetRange()
{
	LPCSTR lpRange;
	RangeS sResult;

	lpRange = GetHeader("Range");
	if (lpRange != NULL)
	{
		LPSTR lpStart, lpDelim;
		char szBuf[128];
		BCPString strLow(lpRange);
		int nIndex;

		strLow.MakeLower();
		nIndex = strLow.Find("=");
		if (nIndex < 0)
		{
			goto _return;
		}
		memzero(szBuf, sizeof(szBuf));
		strncpy(szBuf, lpRange + nIndex + 1, sizeof(szBuf) - 1);
		lpStart = szBuf;
		lpDelim = strchr(lpStart, '-');
		if (lpDelim)
		{
			*lpDelim = 0;
		}
		sResult.bSet = true;
		sResult.nStart = atoll(szBuf);
		if (lpDelim)
		{
			lpDelim++;
			if (BCAscii::IsDigit(*lpDelim))
			{
				sResult.nEnd = atoll(lpDelim);
			}
		}
	}
_return:
	return sResult;
}

uint64_t HTTPResp::GetContentLength()
{
	LPCSTR lpCL;

	lpCL = GetHeader("Content-Length");
	if (lpCL != NULL)
	{
		return atoll(lpCL);
	}
	return 0;
}

///////////////////////////////////////////////////////////////////////////////
// Class : HTTPProtocol
///////////////////////////////////////////////////////////////////////////////

BCStrPtrLen HTTPProtocol::sMethods[] =
{
    BCStrPtrLen("GET"),
    BCStrPtrLen("HEAD"),
    BCStrPtrLen("POST"),
    BCStrPtrLen("OPTIONS"),
    BCStrPtrLen("PUT"),
    BCStrPtrLen("DELETE"),
    BCStrPtrLen("TRACE"),
    BCStrPtrLen("CONNECT"),
};

HTTPMethod HTTPProtocol::GetMethod(const BCStrPtrLen* inMethodStr)
{
    HTTPMethod theMethod = httpIllegalMethod;

    if (inMethodStr->Len == 0)
            return httpIllegalMethod;

    switch((inMethodStr->Ptr)[0])
        {
        case 'G':       theMethod = httpGetMethod; break;
        case 'H':       theMethod = httpHeadMethod; break;
        case 'P':       theMethod = httpPostMethod; break;  // Most likely POST and not PUT
        case 'O':       theMethod = httpOptionsMethod; break;
        case 'D':       theMethod = httpDeleteMethod; break;
        case 'T':       theMethod = httpTraceMethod; break;
        case 'C':       theMethod = httpConnectMethod; break;
        }

    if ( (theMethod != httpIllegalMethod) && (inMethodStr->Equal(sMethods[theMethod])) )
            return theMethod;

    // Check for remaining methods (Only PUT method is left)
    if ( inMethodStr->Equal(sMethods[httpPutMethod]) )
            return httpPutMethod;

    return httpIllegalMethod;
}

BCStrPtrLen HTTPProtocol::sHeaders[] =
{
    BCStrPtrLen("Connection"),
    BCStrPtrLen("Date"),
    BCStrPtrLen("Authorization"),
    BCStrPtrLen("If-Modified-Since"),
    BCStrPtrLen("Server"),
    BCStrPtrLen("WWW-Authenticate"),
    BCStrPtrLen("Expires"),
    BCStrPtrLen("Last-Modified"),

    BCStrPtrLen("Cache-Control"),
    BCStrPtrLen("Pragma"),
    BCStrPtrLen("Trailer"),
    BCStrPtrLen("Transfer-Encoding"),
    BCStrPtrLen("Upgrade"),
    BCStrPtrLen("Via"),
    BCStrPtrLen("Warning"),

    BCStrPtrLen("Accept"),
    BCStrPtrLen("Accept-Charset"),
    BCStrPtrLen("Accept-Encoding"),
    BCStrPtrLen("Accept-Language"),
    BCStrPtrLen("Expect"),
    BCStrPtrLen("From"),
    BCStrPtrLen("Host"),
    BCStrPtrLen("If-Match"),
    BCStrPtrLen("If-None-Match"),
    BCStrPtrLen("If-Range"),
    BCStrPtrLen("If-Unmodified-Since"),
    BCStrPtrLen("Max-Forwards"),
    BCStrPtrLen("Proxy-Authorization"),
    BCStrPtrLen("Range"),
    BCStrPtrLen("Referer"),
    BCStrPtrLen("TE"),
    BCStrPtrLen("User-Agent"),

    BCStrPtrLen("Accept-Ranges"),
    BCStrPtrLen("Age"),
    BCStrPtrLen("ETag"),
    BCStrPtrLen("Location"),
    BCStrPtrLen("Proxy-Authenticate"),
    BCStrPtrLen("Retry-After"),
    BCStrPtrLen("Vary"),

    BCStrPtrLen("Allow"),
    BCStrPtrLen("Content-Encoding"),
    BCStrPtrLen("Content-Language"),
    BCStrPtrLen("Content-Length"),
    BCStrPtrLen("Content-Location"),
    BCStrPtrLen("Content-MD5"),
    BCStrPtrLen("Content-Range"),
    BCStrPtrLen("Content-Type"),

    BCStrPtrLen("X-SessionCookie"),
    BCStrPtrLen("X-Server-IP-Address"),

    BCStrPtrLen(" ,")
};

HTTPHeader HTTPProtocol::GetHeader(const BCStrPtrLen* inHeaderStr)
{
    if (inHeaderStr->Len == 0)
	{
		return httpIllegalHeader;
	}

    HTTPHeader theHeader = httpIllegalHeader;

    //chances are this is one of our selected "VIP" headers. so check for this.
    switch((inHeaderStr->Ptr)[0])
    {
    case 'C':       case 'c':       theHeader = httpConnectionHeader; break;
    case 'S':       case 's':       theHeader = httpServerHeader; break;
    case 'D':       case 'd':       theHeader = httpDateHeader; break;
    case 'A':       case 'a':       theHeader = httpAuthorizationHeader; break;
    case 'W':       case 'w':       theHeader = httpWWWAuthenticateHeader; break;
    case 'I':       case 'i':       theHeader = httpIfModifiedSinceHeader; break;
    case 'E':       case 'e':       theHeader = httpExpiresHeader; break;
    case 'L':       case 'l':       theHeader = httpLastModifiedHeader; break;
    // Added this to optimize for HTTP tunnelling in the server (Not really a VIP header)
    case 'X':       case 'x':       theHeader = httpSessionCookieHeader; break;
    }

    if ((theHeader != httpIllegalHeader) &&
		(inHeaderStr->EqualIgnoreCase(sHeaders[theHeader].Ptr, sHeaders[theHeader].Len)))
	{
		return theHeader;
	}

    //If this isn't one of our VIP headers, go through the remaining request headers, trying
    //to find the right one.
    for (int32_t x = httpNumVIPHeaders; x < httpNumHeaders; x++)
	{
		if (inHeaderStr->EqualIgnoreCase(sHeaders[x].Ptr, sHeaders[x].Len))
		{
			return x;
		}
	}
    return httpIllegalHeader;
}

BCStrPtrLen HTTPProtocol::sStatusCodeStrings[] =
{
    BCStrPtrLen("Continue"),              //kContinue
    BCStrPtrLen("Switching Protocols"),       //kSwitchingProtocols
    BCStrPtrLen("OK"),                //kOK
    BCStrPtrLen("Created"),               //kCreated
    BCStrPtrLen("Accepted"),              //kAccepted
    BCStrPtrLen("Non Authoritative Information"), //kNonAuthoritativeInformation
    BCStrPtrLen("No Content"),            //kNoContent
    BCStrPtrLen("Reset Content"),         //kResetContent
    BCStrPtrLen("Partial Content"),           //kPartialContent
    BCStrPtrLen("Multiple Choices"),          //kMultipleChoices
    BCStrPtrLen("Moved Permanently"),         //kMovedPermanently
    BCStrPtrLen("Found"),             //kFound
    BCStrPtrLen("See Other"),             //kSeeOther
    BCStrPtrLen("Not Modified"),          //kNotModified
    BCStrPtrLen("Use Proxy"),             //kUseProxy
    BCStrPtrLen("Temporary Redirect"),        //kTemporaryRedirect
    BCStrPtrLen("Bad Request"),           //kBadRequest
    BCStrPtrLen("Unauthorized"),          //kUnAuthorized
    BCStrPtrLen("Payment Required"),          //kPaymentRequired
    BCStrPtrLen("Forbidden"),             //kForbidden
    BCStrPtrLen("Not Found"),             //kNotFound
    BCStrPtrLen("Method Not Allowed"),        //kMethodNotAllowed
    BCStrPtrLen("Not Acceptable"),            //kNotAcceptable
    BCStrPtrLen("Proxy Authentication Required"), //kProxyAuthenticationRequired
    BCStrPtrLen("Request Time-out"),          //kRequestTimeout
    BCStrPtrLen("Conflict"),              //kConflict
    BCStrPtrLen("Gone"),              //kGone
    BCStrPtrLen("Length Required"),           //kLengthRequired
    BCStrPtrLen("Precondition Failed"),       //kPreconditionFailed
    BCStrPtrLen("Request Entity Too Large"),      //kRequestEntityTooLarge
    BCStrPtrLen("Request-URI Too Large"),     //kRequestURITooLarge
    BCStrPtrLen("Unsupported Media Type"),        //kUnsupportedMediaType
    BCStrPtrLen("Request Range Not Satisfiable"), //kRequestRangeNotSatisfiable
    BCStrPtrLen("Expectation Failed"),        //kExpectationFailed
    BCStrPtrLen("Internal Server Error"),     //kInternalServerError
    BCStrPtrLen("Not Implemented"),           //kNotImplemented
    BCStrPtrLen("Bad Gateway"),           //kBadGateway
    BCStrPtrLen("Service Unavailable"),       //kServiceUnavailable
    BCStrPtrLen("Gateway Timeout"),           //kGatewayTimeout
    BCStrPtrLen("HTTP Version not supported")     //kHTTPVersionNotSupported
};

int32_t HTTPProtocol::sStatusCodes[] =
{
    100,            //kContinue
    101,            //kSwitchingProtocols
    200,            //kOK
    201,            //kCreated
    202,            //kAccepted
    203,            //kNonAuthoritativeInformation
    204,            //kNoContent
    205,            //kResetContent
    206,            //kPartialContent
    300,            //kMultipleChoices
    301,            //kMovedPermanently
    302,            //kFound
    303,            //kSeeOther
    304,            //kNotModified
    305,            //kUseProxy
    307,            //kTemporaryRedirect
    400,            //kBadRequest
    401,            //kUnAuthorized
    402,            //kPaymentRequired
    403,            //kForbidden
    404,            //kNotFound
    405,            //kMethodNotAllowed
    406,            //kNotAcceptable
    407,            //kProxyAuthenticationRequired
    408,            //kRequestTimeout
    409,            //kConflict
    410,            //kGone
    411,            //kLengthRequired
    412,            //kPreconditionFailed
    413,            //kRequestEntityTooLarge
    414,            //kRequestURITooLarge
    415,            //kUnsupportedMediaType
    416,            //kRequestRangeNotSatisfiable
    417,            //kExpectationFailed
    500,            //kInternalServerError
    501,            //kNotImplemented
    502,            //kBadGateway
    503,            //kServiceUnavailable
    504,            //kGatewayTimeout
    505				//kHTTPVersionNotSupported
};

BCStrPtrLen HTTPProtocol::sStatusCodeAsStrings[] =
{
  BCStrPtrLen("100"),               //kContinue
  BCStrPtrLen("101"),               //kSwitchingProtocols
  BCStrPtrLen("200"),               //kOK
  BCStrPtrLen("201"),               //kCreated
  BCStrPtrLen("202"),               //kAccepted
  BCStrPtrLen("203"),               //kNonAuthoritativeInformation
  BCStrPtrLen("204"),               //kNoContent
  BCStrPtrLen("205"),               //kResetContent
  BCStrPtrLen("206"),               //kPartialContent
  BCStrPtrLen("300"),               //kMultipleChoices
  BCStrPtrLen("301"),               //kMovedPermanently
  BCStrPtrLen("302"),               //kFound
  BCStrPtrLen("303"),               //kSeeOther
  BCStrPtrLen("304"),               //kNotModified
  BCStrPtrLen("305"),               //kUseProxy
  BCStrPtrLen("307"),               //kTemporaryRedirect
  BCStrPtrLen("400"),               //kBadRequest
  BCStrPtrLen("401"),               //kUnAuthorized
  BCStrPtrLen("402"),               //kPaymentRequired
  BCStrPtrLen("403"),               //kForbidden
  BCStrPtrLen("404"),               //kNotFound
  BCStrPtrLen("405"),               //kMethodNotAllowed
  BCStrPtrLen("406"),               //kNotAcceptable
  BCStrPtrLen("407"),               //kProxyAuthenticationRequired
  BCStrPtrLen("408"),               //kRequestTimeout
  BCStrPtrLen("409"),               //kConflict
  BCStrPtrLen("410"),               //kGone
  BCStrPtrLen("411"),               //kLengthRequired
  BCStrPtrLen("412"),               //kPreconditionFailed
  BCStrPtrLen("413"),               //kRequestEntityTooLarge
  BCStrPtrLen("414"),               //kRequestURITooLarge
  BCStrPtrLen("415"),               //kUnsupportedMediaType
  BCStrPtrLen("416"),               //kRequestRangeNotSatisfiable
  BCStrPtrLen("417"),               //kExpectationFailed
  BCStrPtrLen("500"),               //kInternalServerError
  BCStrPtrLen("501"),               //kNotImplemented
  BCStrPtrLen("502"),               //kBadGateway
  BCStrPtrLen("503"),               //kServiceUnavailable
  BCStrPtrLen("504"),               //kGatewayTimeout
  BCStrPtrLen("505")                //kHTTPVersionNotSupported
};

BCStrPtrLen HTTPProtocol::sVersionStrings[] =
{
    BCStrPtrLen("HTTP/0.9"),
    BCStrPtrLen("HTTP/1.0"),
    BCStrPtrLen("HTTP/1.1")
};

HTTPVersion HTTPProtocol::GetVersion(BCStrPtrLen* versionStr)
{
    if (versionStr->Len != 8)
            return httpIllegalVersion;
    int32_t limit = httpNumVersions;
    for (int32_t x = 0; x < limit; x++)
    {
        if (versionStr->EqualIgnoreCase(sVersionStrings[x].Ptr, sVersionStrings[x].Len))
            return x;
    }

    return httpIllegalVersion;
}

int32_t HTTPProtocol::GetStatusCodeByIETFCode(int inStat)
{
	int index = -1;
	for (uint32_t i = 0; i < sizeof(sStatusCodes) / sizeof(sStatusCodes[0]); i++)
	{
		if (sStatusCodes[i] == inStat)
		{
			index = i;
			break;
		}
	}
	return index;
}

BCRESULT HTTPProtocol::ParseHost(
	LPCSTR lpszUrl, 
	BCStrPtrLen &refStrHost, 
	BCStrPtrLen &refStrPort,
	BCStrPtrLen &refRelativeUrl,
	bool& bSSL,
	uint16_t &refPort)
{
	uint16_t defaultPort = 80;

	ASSERT(lpszUrl != 0);

	// Lower protocol characters
	char szLower[7];
	char *p, *temp;
	const char *pUrl = lpszUrl;

	for (p = szLower; p < szLower + 6; p++, pUrl++)
	{
		*p = tolower(*pUrl);
	}
	*p = 0;

	// look for usual :// pattern
	p = (char *)strstr(lpszUrl, "://");
	int len = (int)(p - lpszUrl);
	if (p == 0)
	{
		return false;
	}

	bSSL = false;
	if (len == 2 && strncmp(szLower, "ws", 2)==0)
	{
		goto parsehost;
	}
	else if (len == 3 && strncmp(szLower, "wss", 3)==0)
	{
		defaultPort = 443;
		bSSL = true;
		goto parsehost;
	}
	else if (len == 4 && strncmp(szLower, "http", 4)==0)
	{
		goto parsehost;
	}
	else if (len == 5 && strncmp(szLower, "https", 5)==0)
	{
		defaultPort = 443;
		bSSL = true;
		goto parsehost;
	}
	else
	{
		return BC_R_FAILURE;
	}

parsehost:
	// lets get the hostname
	p+=3;

	// check for sudden death
	if (*p==0)
	{
		return false;
	}

	int iEnd   = strlen(p);
	int iCol   = iEnd+1;
	int iQues  = iEnd+1;
	int iSlash = iEnd+1;

	if ((temp=strstr(p, ":"))!=0)
		iCol = temp-p;
	if ((temp=strstr(p, "?"))!=0)
		iQues = temp-p;
	if ((temp=strstr(p, "/"))!=0)
		iSlash = temp-p;

	int min = iSlash < iEnd ? iSlash : iEnd;
	min = iQues   < min ? iQues   : min;
	
	//if (min == iEnd + 1) // iEnd is host last character
	//{
	//	refStrHost.Ptr = p;
	//	refStrHost.Len = (size_t)iEnd;
	//	return BC_R_SUCCESS;
	//}

	int hostlen= iCol < min ? iCol : min;

	if (min < 256)
	{
		refStrHost.Ptr = p;
		refStrHost.Len = (size_t)hostlen;
	}
	else
	{
		return BC_R_FAILURE;
	}

	p += hostlen;
	iEnd -= hostlen;

	// get the port number if available
	if (*p == ':')
	{
		p++;
		iEnd--;

		int portlen = min-hostlen-1;
		if (portlen < 6)
		{
			refStrPort.Ptr = p;
			refStrPort.Len = (size_t)portlen;
		}
		else
		{
			return BC_R_FAILURE;
		}

		p += portlen;
		iEnd -= portlen;
	}
	if (iEnd > 0)
	{
		refRelativeUrl.Ptr = ++p;
		refRelativeUrl.Len = --iEnd;
	}
	else
	{
		refRelativeUrl.Ptr = NULL;
		refRelativeUrl.Len = 0;
	}
	if (refStrPort.Len > 0)
	{
		char szPort[6] = { 0 };

		strncpy(szPort, refStrPort.Ptr, refStrPort.Len);
		refPort = (uint16_t)::atoi(szPort);
	}
	else
	{
		refPort = defaultPort;
	}

	return BC_R_SUCCESS;
}

///////////////////////////////////////////////////////////////////////////////
// url encode : 
///////////////////////////////////////////////////////////////////////////////

static const char* G_szURLEncodeCharsMap[] = 
{
    /* 00 */"%00","%01","%02","%03","%04","%05","%06","%07",
    /* 08 */"%08","%09","%0A","%0B","%0C","%0D","%0E","%0F",
    /* 10 */"%10","%11","%12","%13","%14","%15","%16","%17",
    /* 18 */"%18","%19","%1A","%1B","%1C","%1D","%1E","%1F",
    /* 20 */"+","%21","%22","%23","%24","%25","%26","%27",
    /* 28 */"%28","%29","%2A","%2B","%2C","%2D","%2E","%2F",
    /* 30 */"0","1","2","3","4","5","6","7",
    /* 38 */"8","9","%3A","%3B","%3C","%3D","%3E","%3F",
    /* 40 */"%40","A","B","C","D","E","F","G",
    /* 48 */"H","I","J","K","L","M","N","O",
    /* 50 */"P","Q","R","S","T","U","V","W",
    /* 58 */"X","Y","Z","%5B","%5C","%5D","%5E","%5F",
    /* 60 */"%60","a","b","c","d","e","f","g",
    /* 68 */"h","i","j","k","l","m","n","o",
    /* 70 */"p","q","r","s","t","u","v","w",
    /* 78 */"x","y","z","%7B","%7C","%7D","%7E","%7F",

    /* 80 */"%80", "%81", "%82", "%83", "%84", "%85", "%86", "%87", "%88", "%89", "%8A", "%8B", "%8C", "%8D", "%8E", "%8F",
    "%90", "%91", "%92", "%93", "%94", "%95", "%96", "%97", "%98", "%99", "%9A", "%9B", "%9C", "%9D", "%9E", "%9F",
    "%A0", "%A1", "%A2", "%A3", "%A4", "%A5", "%A6", "%A7", "%A8", "%A9", "%AA", "%AB", "%AC", "%AD", "%AE", "%AF",
    "%B0", "%B1", "%B2", "%B3", "%B4", "%B5", "%B6", "%B7", "%B8", "%B9", "%BA", "%BB", "%BC", "%BD", "%BE", "%BF",
    "%C0", "%C1", "%C2", "%C3", "%C4", "%C5", "%C6", "%C7", "%C8", "%C9", "%CA", "%CB", "%CC", "%CD", "%CE", "%CF",
    "%D0", "%D1", "%D2", "%D3", "%D4", "%D5", "%D6", "%D7", "%D8", "%D9", "%DA", "%DB", "%DC", "%DD", "%DE", "%DF",
    "%E0", "%E1", "%E2", "%E3", "%E4", "%E5", "%E6", "%E7", "%E8", "%E9", "%EA", "%EB", "%EC", "%ED", "%EE", "%EF",
    "%F0", "%F1", "%F2", "%F3", "%F4", "%F5", "%F6", "%F7", "%F8", "%F9", "%FA", "%FB", "%FC", "%FD", "%FE", "%FF",
};

static const int G_nURLEncodeCharsLenMap[] = 
{
    /* 00 */3,3,3,3,3,3,3,3,
    /* 08 */3,3,3,3,3,3,3,3,
    /* 10 */3,3,3,3,3,3,3,3,
    /* 18 */3,3,3,3,3,3,3,3,
    /* 20 */1,3,3,3,3,3,3,3,
    /* 28 */3,3,3,3,3,3,3,3,
    /* 30 */1,1,1,1,1,1,1,1,
    /* 38 */1,1,3,3,3,3,3,3,
    /* 40 */3,1,1,1,1,1,1,1,
    /* 48 */1,1,1,1,1,1,1,1,
    /* 50 */1,1,1,1,1,1,1,1,
    /* 58 */1,1,1,3,3,3,3,3,
    /* 60 */3,1,1,1,1,1,1,1,
    /* 68 */1,1,1,1,1,1,1,1,
    /* 70 */1,1,1,1,1,1,1,1,
    /* 78 */1,1,1,3,3,3,3,3,

    /* 80 */3,3,3,3,3,3,3,3, 3,3,3,3,3,3,3,3,
    3,3,3,3,3,3,3,3, 3,3,3,3,3,3,3,3,
    3,3,3,3,3,3,3,3, 3,3,3,3,3,3,3,3,
    3,3,3,3,3,3,3,3, 3,3,3,3,3,3,3,3,
    3,3,3,3,3,3,3,3, 3,3,3,3,3,3,3,3,
    3,3,3,3,3,3,3,3, 3,3,3,3,3,3,3,3,
    3,3,3,3,3,3,3,3, 3,3,3,3,3,3,3,3,
    3,3,3,3,3,3,3,3, 3,3,3,3,3,3,3,3,
};

/* URL编码
 *  * [in]lpszIn  - 输入的地址字符串
 *   * [in] lpszOut - 输出字符串存储的位置
 *    * [in] size_out - 输出字符串目标内存区的大小，如果为0，表示没有长度限制
 *     * [out]lpszOut - 存储编码后的地址字符串
 *      * [return]返回地址字符串的长度
 *       */
size_t HTTPProtocol::URLEncode(const char *lpszIn, char *lpszOut, size_t size_out)
{
    if (!lpszOut)
        return 0;
    if (!lpszIn)
    {
        *lpszOut = '\0';
        return 0;
    }
    
    const char *p_src = lpszIn;
    char *p_dest = lpszOut;
    char *p_dest_end = NULL;
    if (size_out > 0)
        p_dest_end = lpszOut + size_out;

    unsigned char b = (unsigned char)*p_src++;
    int len;
    while (b)
    {
        len = G_nURLEncodeCharsLenMap[b];
        if (p_dest_end && (p_dest + len + 1 >= p_dest_end))
        {
            *p_dest = '\0';
            return (p_dest - lpszOut);
        }
        memcpy(p_dest, G_szURLEncodeCharsMap[b], len); 
        p_dest += len;
        b = (unsigned char)*p_src++;
    }
    
    *p_dest = '\0';
    return (p_dest - lpszOut);
}

static const char* URLENCODE_CHARS_RFC[] = 
{
	/* 00 00*/"%00","%01","%02","%03","%04","%05","%06","%07",
	/* 08 08*/"%08","%09","%0A","%0B","%0C","%0D","%0E","%0F",
	/* 10 16*/"%10","%11","%12","%13","%14","%15","%16","%17",
	/* 18 24*/"%18","%19","%1A","%1B","%1C","%1D","%1E","%1F",
	/* 20 32*/"%20","!","%22","#","$","%25","&","'",
	/* 28 40*/"(",")","*","%2b",",","-",".","/",
	/* 30 48*/"0","1","2","3","4","5","6","7",
	/* 38 56*/"8","9",":",";","%3C","=","%3E","?",
	/* 40 64*/"@","A","B","C","D","E","F","G",
	/* 48 72*/"H","I","J","K","L","M","N","O",
	/* 50 80*/"P","Q","R","S","T","U","V","W",
	/* 58 88*/"X","Y","Z","[","%5C","]","%5E","_",
	/* 60 96*/"%60","a","b","c","d","e","f","g",
	/* 68 104*/"h","i","j","k","l","m","n","o",
	/* 70 112*/"p","q","r","s","t","u","v","w",
	/* 78 120*/"x","y","z","%7B","%7C","%7D","~","%7F",

	/* 80 128*/"%80", "%81", "%82", "%83", "%84", "%85", "%86", "%87", "%88", "%89", "%8A", "%8B", "%8C", "%8D", "%8E", "%8F",
	/* 80 144*/"%90", "%91", "%92", "%93", "%94", "%95", "%96", "%97", "%98", "%99", "%9A", "%9B", "%9C", "%9D", "%9E", "%9F",
	/* 80 160*/"%A0", "%A1", "%A2", "%A3", "%A4", "%A5", "%A6", "%A7", "%A8", "%A9", "%AA", "%AB", "%AC", "%AD", "%AE", "%AF",
	/* 80 176*/"%B0", "%B1", "%B2", "%B3", "%B4", "%B5", "%B6", "%B7", "%B8", "%B9", "%BA", "%BB", "%BC", "%BD", "%BE", "%BF",
	/* 80 192*/"%C0", "%C1", "%C2", "%C3", "%C4", "%C5", "%C6", "%C7", "%C8", "%C9", "%CA", "%CB", "%CC", "%CD", "%CE", "%CF",
	/* 80 208*/"%D0", "%D1", "%D2", "%D3", "%D4", "%D5", "%D6", "%D7", "%D8", "%D9", "%DA", "%DB", "%DC", "%DD", "%DE", "%DF",
	/* 80 224*/"%E0", "%E1", "%E2", "%E3", "%E4", "%E5", "%E6", "%E7", "%E8", "%E9", "%EA", "%EB", "%EC", "%ED", "%EE", "%EF",
	/* 80 240*/"%F0", "%F1", "%F2", "%F3", "%F4", "%F5", "%F6", "%F7", "%F8", "%F9", "%FA", "%FB", "%FC", "%FD", "%FE", "%FF",
};

static const int URLENCODE_CHARS_LEN_RFC[] = 
{
	/* 00 */3,3,3,3,3,3,3,3,
	/* 08 */3,3,3,3,3,3,3,3,
	/* 10 */3,3,3,3,3,3,3,3,
	/* 18 */3,3,3,3,3,3,3,3,
	/* 20 */3,1,3,1,1,3,1,1,
	/* 28 */1,1,1,3,1,1,1,1,
	/* 30 */1,1,1,1,1,1,1,1,
	/* 38 */1,1,1,1,3,1,3,1,
	/* 40 */1,1,1,1,1,1,1,1,
	/* 48 */1,1,1,1,1,1,1,1,
	/* 50 */1,1,1,1,1,1,1,1,
	/* 58 */1,1,1,1,3,1,3,1,
	/* 60 */3,1,1,1,1,1,1,1,
	/* 68 */1,1,1,1,1,1,1,1,
	/* 70 */1,1,1,1,1,1,1,1,
	/* 78 */1,1,1,3,3,3,1,3,

	/* 80 */3,3,3,3,3,3,3,3, 3,3,3,3,3,3,3,3,
	3,3,3,3,3,3,3,3, 3,3,3,3,3,3,3,3,
	3,3,3,3,3,3,3,3, 3,3,3,3,3,3,3,3,
	3,3,3,3,3,3,3,3, 3,3,3,3,3,3,3,3,
	3,3,3,3,3,3,3,3, 3,3,3,3,3,3,3,3,
	3,3,3,3,3,3,3,3, 3,3,3,3,3,3,3,3,
	3,3,3,3,3,3,3,3, 3,3,3,3,3,3,3,3,
	3,3,3,3,3,3,3,3, 3,3,3,3,3,3,3,3,
};

size_t HTTPProtocol::URLEncode_RFC(const char *lpszIn, char *lpszOut, size_t size_out)
{
	if (!lpszOut)
		return 0;
	if (!lpszIn)
	{
		*lpszOut = '\0';
		return 0;
	}

	const char *p_src = lpszIn;
	char *p_dest = lpszOut;
	char *p_dest_end = NULL;
	if (size_out > 0)
		p_dest_end = lpszOut + size_out;

	unsigned char b = (unsigned char)*p_src++;
	int len;
	while (b)
	{
		len = URLENCODE_CHARS_LEN_RFC[b];
		if (p_dest_end && (p_dest + len + 1 >= p_dest_end))
		{
			*p_dest = '\0';
			return (p_dest - lpszOut);
		}
		memcpy(p_dest, URLENCODE_CHARS_RFC[b], len);
		p_dest += len;
		b = (unsigned char)*p_src++;
	}

	*p_dest = '\0';
	return (p_dest - lpszOut);
}

static const BYTE G_xHexCharsMap[] =
{
	/*00*/0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	/*10*/0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	/*20*/0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	/*30*/0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	/*40*/0xFF, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	/*50*/0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	/*60*/0xFF, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	/*70*/0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	/*80*/0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	/*90*/0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	/*A0*/0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	/*B0*/0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	/*C0*/0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	/*D0*/0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	/*E0*/0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	/*F0*/0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};

static size_t RemoveHalfGbkCode(char *lpszInput)
{
	char *lpszSave = lpszInput;
	char *lpszScan = lpszInput;

	bool in_gbk_char = false;
	char *previous_amp = NULL;
	while (*lpszScan)
	{
		if ((*lpszScan & 0x80) || in_gbk_char)
			in_gbk_char = !in_gbk_char;

		if (*lpszScan == '&')
			previous_amp = lpszSave;
		else if (*lpszScan == ';')
			previous_amp = NULL;

		*lpszSave++ = *lpszScan++;
	}

	if (in_gbk_char)
		lpszSave--;
	if (previous_amp)
		lpszSave = previous_amp;

	*lpszSave = '\0';
	return (size_t)(lpszSave - lpszInput);
}

/* URL解码
 *  * [in]lpszIn  - 包含地址解码前地址内容的字符串
 *   * [in] lpszOut - 输出字符串存储的位置
 *    * [in] size_out - 输出字符串目标内存区的大小，如果为0，表示没有长度限制
 *     * [out]lpszOut - 返回的地址字符串
 *      * [return]返回地址字符串的长度
 *       */
size_t HTTPProtocol::URLDecode(const char *lpszIn, char *lpszOut, size_t size_out)
{
	if (!lpszOut)
		return 0;
	if (!lpszIn)
		{
			*lpszOut = '\0';
			return 0;
		}

	char *lpszWrite = lpszOut;
	char *lpszWriteEnd = (size_out > 0) ? (lpszOut + size_out) : NULL;

	const char *pRead = lpszIn;
	signed char ch1, ch2;

	while (*pRead)
	{
		if (*pRead=='%')
		{
			pRead++;
			ch1 = (signed char)G_xHexCharsMap[(BYTE)*pRead];
			if (ch1 == -1)
				continue;

			pRead++;
			ch2 = (signed char)G_xHexCharsMap[(BYTE)*pRead];
			if (ch2 == -1)
				continue;

			*lpszWrite++ = (ch1 << 4) | ch2;
		}
		else if (*pRead=='+')
		{
			*lpszWrite++ = ' ';
		}
		else 
		{
			*lpszWrite++ = *pRead;
		}
		pRead++;

		if (lpszWriteEnd && (lpszWrite + 1 >= lpszWriteEnd))
		{
			break;
		}
	}

	*lpszWrite = '\0';

	if (lpszWriteEnd && (lpszWrite + 1 >= lpszWriteEnd))
		return RemoveHalfGbkCode(lpszOut);
	else
		return (size_t)(lpszWrite - lpszOut);
}

BCRESULT HTTPProtocol::ParseAddrFromUrl(
	LPCSTR lpszHttpUrl,
	std::string& strIP,
	uint16_t& refPort,
	int32_t& refNetType,
	std::string& strRelativeUrl,
	bool& bSSL)
{
	BCRESULT result;
	BCStrPtrLen strHost, strPort, strRelaUrl;
	char szHost[128] = {0};
	char ipBuf[64] = { 0 };

	ASSERT(lpszHttpUrl);

	result = ParseHost(lpszHttpUrl, strHost, strPort, strRelaUrl, bSSL, refPort);
	if (result != BC_R_SUCCESS)
	{
		goto quit;
	}
	strncpy(szHost, strHost.Ptr, strHost.Len);
	szHost[strHost.Len] = '\0';
	// Get ip
	result = DNSGetAddrInfo(szHost, ipBuf, sizeof(ipBuf), refNetType);
	if (result != BC_R_SUCCESS)
	{
		goto quit;
	}
	strIP = ipBuf;
	strRelativeUrl.append(strRelaUrl.Ptr, strRelaUrl.Len);

	return BC_R_SUCCESS;

quit:
	return result;
}

BCRESULT HTTPProtocol::ParseLocationWithParams(
	LPCSTR lpszLocation,
	std::string& strPath,
	std::map<std::string, std::string>& params)
{
	std::vector<std::string> items;
	SplitString(lpszLocation, "?", &items);
	if (items.size() > 0)
	{
		strPath = items[0];
		if (items.size() > 1)
		{
			std::string strParams = items[1];
			items.clear();
			SplitString(strParams, "&", &items);
			for (auto& it : items)
			{
				std::vector<std::string> kvpair;
				SplitStringByFirstDelimiter(it, "=", &kvpair);
				if (kvpair.size() == 2)
				{
					params[kvpair[0]] = kvpair[1];
				}
			}
		}
	}
	else
	{
		strPath = lpszLocation;
	}
	return BC_R_SUCCESS;
}

///////////////////////////////////////////////////////////////////////////////
// class : HTTPChunkParser
///////////////////////////////////////////////////////////////////////////////

static const char	G_szCRLF[] = "\r\n";
static const char	G_szChunkEnd[] = "\r\n\r\n";

HTTPChunkParser::HTTPChunkParser()
	: m_pHandler(NULL)
	, m_nChunkSize(0)
	, m_sReader(&m_sChunk)
	, m_eRecvState(RECV_SIZE)
	, m_nRequireDataSize(1)
	, m_bChunkParseCtrl(false)
{
	//
}

HTTPChunkParser::~HTTPChunkParser()
{
	//
}

BCRESULT HTTPChunkParser::Create(IHTTPChunkHandler *pHandler)
{
	ASSERT(pHandler != NULL);
	m_pHandler = pHandler;

	return BC_R_SUCCESS;
}

BOOL HTTPChunkParser::Parse()
{
	BOOL bContinue = TRUE;
	if(!m_bChunkParseCtrl)
	{
		m_bChunkParseCtrl = true;
		while(bContinue)
		{
			if (m_nRequireDataSize > m_sChunk.RemainingLength())
			{
				m_sChunk.RemoveConsumed();
				m_bChunkParseCtrl = false;
				return TRUE;
			}
			switch(m_eRecvState)
			{
				case RECV_SIZE:
					bContinue = _ParseSize();
					if(m_nChunkSize == 0)
					{
						m_pHandler->OnChunkRecv(new BCBuffer);
						m_sChunk.Reset();
						m_bChunkParseCtrl = false;
						return FALSE;
					}
					break;
				case RECV_CHUNK:
					bContinue = _FinishChunk();
					break;
			}
		}
		if (m_sChunk.RemainingLength() == 0)
		{
			m_sChunk.Reset();
		}
		else
		{
			m_sChunk.RemoveConsumed();
		}
		m_bChunkParseCtrl = false;
	}
	return FALSE;
}

BCBuffer *HTTPChunkParser::GetRecvBuf()
{
	return &m_sChunk;
}

void HTTPChunkParser::Cleanup()
{
	m_sChunk.Reset(0);
	m_sChunk.RemoveConsumed();
}

BOOL HTTPChunkParser::_RequireData(
	uint32_t nSize,
	uint32_t nForward,
	RecvStateE eRecvState)
{
	UNUSED(nForward);
	// Change state needs previous process finished.
	if (m_nRequireDataSize == 0)
	{
		m_eRecvState = eRecvState;
	}
	m_nRequireDataSize += nSize;
	if (m_sChunk.RemainingLength() >= m_nRequireDataSize)
	{
		return TRUE;
	}
	return FALSE;
}

BOOL HTTPChunkParser::_RequireSize()
{
	return _RequireData(1, 0, RECV_SIZE);
}

BOOL HTTPChunkParser::_RequireChunk()
{
	return _RequireData(m_nChunkSize + 2, 0, RECV_CHUNK);
}

BOOL HTTPChunkParser::_ParseSize()
{
	LPCSTR lpStart, lpCurrent;
	size_t nSize;

	lpCurrent = (LPCSTR)m_sChunk.Current();
	lpStart = strstr(lpCurrent, G_szCRLF);
	if (lpStart == NULL)
	{
		return _RequireData(1, 0, RECV_SIZE);
	}
	m_nChunkSize = _Hex2Int(lpCurrent);
	if (m_nChunkSize > 0)
	{
		nSize = (lpStart - lpCurrent + 2); // Skip size & CRLF
		m_nRequireDataSize = 0;
		//ASSERT(m_sReader.RemainingLength() >= nSize);
		m_sReader.Skip(nSize);

		return _RequireChunk();
	} 
	else
	{
		LPCSTR lpChunkEnd = strstr(lpCurrent, G_szChunkEnd);
		if (lpChunkEnd)
		{
			return TRUE;
		}
	}
	return FALSE;
}

BOOL HTTPChunkParser::_FinishChunk()
{
//	ASSERT(m_sChunk.RemainingLength() >= m_nChunkSize);

	if (m_sChunk.RemainingLength() >= m_nChunkSize)
	{
		BCBuffer *pBuffer = new BCBuffer();

		m_sChunk.RemoveConsumed();
		pBuffer->ReadFrom(m_sReader, m_nChunkSize);
		m_pHandler->OnChunkRecv(pBuffer);
		m_sReader.Skip(2); // Skip CRLF
		m_sChunk.RemoveConsumed();
		ASSERT(m_nRequireDataSize >= m_nChunkSize + 2);
		m_nRequireDataSize -= (m_nChunkSize + 2);
	}
	else
	{
		return _RequireChunk();
	}

	return _RequireData(1, 0, RECV_SIZE);
}

uint64_t HTTPChunkParser::_Hex2Int(LPCSTR lpInput)
{
	uint64_t nResult = 0;
	char ch;

	while ((ch = *lpInput) != '\0')
	{
		if (ch >= '0' && ch <= '9')
		{
			nResult = nResult*16 + *lpInput-'0';
		}
		else if (ch >= 'a' && ch <= 'f')
		{
			nResult = nResult*16 + *lpInput-'a'+10;
		}
		else if (ch >= 'A' && ch <= 'F')
		{
			nResult = nResult*16 + *lpInput-'A'+10;
		}
		else
		{
			goto quit;
		}
		lpInput++;
	}
quit:
	return nResult;
}

///////////////////////////////////////////////////////////////////////////////
// class : HTTPNotifier
///////////////////////////////////////////////////////////////////////////////

IMPLEMENT_FIXED_ALLOC(HTTPNotifier, 100);

HTTPNotifier::HTTPNotifier()
	: m_eType(0)
	, m_pAction(NULL)
	, m_wParam(NULL)
	, m_lParam(0)
{
	//
}

HTTPNotifier::HTTPNotifier(
	uint32_t eType,
	LPFN_HTTPNotification pAction,
	void *wParam,
	uint64_t lParam)
		: m_eType(eType)
		, m_pAction(pAction)
		, m_wParam(wParam)
		, m_lParam(lParam)
{
	//
}

HTTPNotifier::~HTTPNotifier()
{
	//
}

HTTPNotifier &HTTPNotifier::operator = (const HTTPNotifier &refOther)
{
	m_eType		= refOther.m_eType;
	m_pAction	= refOther.m_pAction;
	m_wParam	= refOther.m_wParam;
	m_lParam	= refOther.m_lParam;

	return *this;
}

void HTTPNotifier::Init(
	uint32_t eType,
	LPFN_HTTPNotification pAction,
	void *wParam,
	uint64_t lParam)
{
	m_eType					= eType;
	m_pAction				= pAction;
	m_wParam				= wParam;
	m_lParam				= lParam;
}

void HTTPNotifier::Notify()
{
	if (m_pAction != NULL)
	{
		(m_pAction)(*this);
	}
}

///////////////////////////////////////////////////////////////////////////////
// class : SendItem
///////////////////////////////////////////////////////////////////////////////

IMPLEMENT_FIXED_ALLOC(HTTPMItem, 100);

HTTPMItem::HTTPMItem(uint32_t eType /*= HTTPM_UNKNOWN*/)
	: m_eType(eType)
{
	//
}

HTTPMItem::~HTTPMItem()
{
	//
}

///////////////////////////////////////////////////////////////////////////////
// class : SendBuffer
///////////////////////////////////////////////////////////////////////////////

HTTPMQueue::HTTPMQueue()
{
	//
}

HTTPMQueue::~HTTPMQueue()
{
	Cleanup();
}

BOOL HTTPMQueue::Append(BCBuffer &refBody)
{
	BOOL bNewItem;
	HTTPMItem *pNewItem;

	pNewItem = NULL;
	bNewItem = FALSE;
	if (refBody.RemainingLength() > 0)
	{
		HTTPMItem *pBack;

		pBack = m_lstItems.Back();
		if (pBack != NULL && pBack->m_eType == HTTPM_USERDATA)
		{
			pNewItem = pBack;
		}
		else
		{
			pNewItem = new HTTPMItem(HTTPM_USERDATA);
			bNewItem = TRUE;
		}
		ASSERT(pNewItem != NULL);
		BCBOStream sWriter(&pNewItem->m_sBuffer);
		refBody.WriteTo(sWriter);
	}
	if (pNewItem != NULL && bNewItem)
	{
		m_lstItems.PushBack(pNewItem);
	}

	return TRUE;
}

BOOL HTTPMQueue::AppendEx(BCBuffer &refBody)
{
	if (refBody.RemainingLength() > 0)
	{
		HTTPMItem *pNewItem;
		pNewItem = new HTTPMItem(HTTPM_USERDATA);
		ASSERT(pNewItem != NULL);
		if (pNewItem != NULL)
		{
			refBody.RemoveConsumed();
			refBody.Extract(&pNewItem->m_sBuffer, INFINITE);
			m_lstItems.PushBack(pNewItem);
		}
	}

	return TRUE;
}

BOOL HTTPMQueue::Append(HTTPNotifier *pNotifier)
{
	HTTPMItem *pNewItem;

	pNewItem = new HTTPMItem(HTTPM_NOTIFIER);
	ASSERT(pNewItem != NULL);
	pNewItem->m_sNotifier = *pNotifier;
	m_lstItems.PushBack(pNewItem);
	return TRUE;
}

HTTPMItem *HTTPMQueue::PopFront()
{
	return m_lstItems.PopFront();
}

void HTTPMQueue::Cleanup()
{
	HTTPMItem *pItem;
	while((pItem = m_lstItems.PopFront()) != NULL)
	{
		delete pItem;
	}
}

BOOL HTTPMQueue::IsEmpty() const
{
	return m_lstItems.IsEmpty();
}

///////////////////////////////////////////////////////////////////////////////
// End of namespace : HTTP
///////////////////////////////////////////////////////////////////////////////

} // End of namespace HTTP

///////////////////////////////////////////////////////////////////////////////
// End of file : HTTPProtocol.cpp
///////////////////////////////////////////////////////////////////////////////
