///////////////////////////////////////////////////////////////////////////////
// file : RTMPUrl.cpp
// author : antoniozhou
///////////////////////////////////////////////////////////////////////////////

#include <regex>
#include <BC/BCLog.h>
#include <BC/BCNet.h>
#include <BC/BCStrPtrLen.h>
#include <RTMP/RTMPUrl.h>



///////////////////////////////////////////////////////////////////////////////
// Namespace : RTMP
///////////////////////////////////////////////////////////////////////////////

namespace RTMP
{

static const BCStrPtrLen kSDK_IOS("fmlivesdk-ios");
static const BCStrPtrLen kSDK_ANDROID("fmlivesdk-android");
static const BCStrPtrLen kSDK_WIN32("fmlivesdk-win32");
static const BCStrPtrLen kFMS_WIN("fms-win32");
static const BCStrPtrLen kFMS_LINUX("fms-linux");


static bool	ParseClientVersion(LPCSTR lpszVer, ClientVersionS &refVer)
{
	/////////////////////////////////////////////////////////////////////////
	//std::regex_match
	//std::regex_match: �������ʽ��Ҫƥ�������ַ�������, Ҳ����˵�������ʽҪ��
	//�ַ�����ȫƥ��, ���, ���ǵ���ƥ��, ����ƥ��ʧ��. 
	//����, �������Ի�ȡ��ƥ�����

	//std::string text = "FMS-win32/1.0.804-build(2020-04-29 14:39:40)";
	std::string text = lpszVer;

	//�����������ʽ
	//���� "()" ���ڲ�����, ������ı���ǰ��� "(" ���ֵ�˳��, ������, ��1��ʼ���б�ŵ� 
	std::string pattern = "^(\\w+)-(\\w+)\\/(\\d+)\\.(\\d+)\\.(\\d+).*";
	std::regex express(pattern);

	//ƥ��
	/*ģ�庯��1-1*/
	//��0��һ���������������ʽƥ����, ���������ǲ�����Ľ��
	//����ʹ�õ��� std::string::iterator ������, �� begin()/ end() ���صĵ���������(std::string::iterator)Ҫһ��
	std::match_results<std::string::iterator> results1;
	if (std::regex_match(text.begin(), text.end(), results1, express))
	{
		if (results1.size() >= 6)
		{
			BCPString strProduct = results1[1].str().c_str();
			strProduct.MakeLower();
			BCPString strPlatform = results1[2].str().c_str();
			strPlatform.MakeLower();
			refVer.product = strProduct.c_str();
			refVer.platform = strPlatform.c_str();
			refVer.majorVersion = ::atoi(results1[3].str().c_str());
			refVer.minorVersion = ::atoi(results1[4].str().c_str());
			refVer.patchVersion = ::atoi(results1[5].str().c_str());
			refVer.version = (((uint32_t)refVer.majorVersion) << 24) + 
				(((uint32_t)refVer.minorVersion) << 16) + refVer.patchVersion;
			return true;
		}
	}
	return false;
}

///////////////////////////////////////////////////////////////////////////////
// class : RTMPUrl
///////////////////////////////////////////////////////////////////////////////

RTMPUrl::RTMPUrl()
	: m_bFPad(false)
	, m_nCapabilities(0)
	, m_nAudioCodecs(0)
	, m_nVideoCodecs(0)
	, m_nVideoFunction(0)
	, m_nObjectEncoding(ObjectEncoding::AMF0)
	, m_nSecureId(0)
	, m_eProtocol(RTMP_PROTOCOL_UNDEFINED)
	, m_nPort(1935)
	, m_eClientOS(CLIENT_OS_UNKNOWN)
	, m_pDataProp(NULL)
	, m_pArgs(NULL)
{
	m_strFlashVer = "WIN 10,0,2,54";
	m_strSwfUrl.resize(MAX_PATH);
	RandomString((void *)m_strSwfUrl.data(), MAX_PATH);
	m_strTcUrl = "rtmp://127.0.0.1/anyapp";
	m_nSecureId = CheckSum(m_strSwfUrl.c_str(), m_strSwfUrl.length());
}

RTMPUrl::~RTMPUrl()
{
	BC_SAFE_DELETE_PTR(m_pDataProp);
	BC_SAFE_DELETE_PTR(m_pArgs);
}

uint16_t RTMPUrl::CheckSum(LPCVOID lpData, uint32_t nSize)
{
	uint32_t nSum;
	uint8_t nByte;
	uint16_t nShort;
	BCFBIStream sReader(lpData, nSize);

	nSum = 0;
	while(!sReader.Eof())
	{
		if (sReader.RemainingLength() == 1)
		{
			sReader.ReadUInt8(&nByte);
			nSum += nByte;
		}
		else
		{
			sReader.ReadUInt16BE(&nShort);
			nSum += nShort;
		}
	}

	/* add back carry outs from top 16 bits to low 16 bits */
	nSum = (nSum >> 16) + (nSum & 0xffff);     /* add hi 16 to low 16 */
	nSum += (nSum >> 16);                     /* add carry */
	return ~nSum; /* truncate to 16 bits */
}

bool RTMPUrl::DecodeUrl(AMFVarPtr pProperty)
{
	AMFVarPtr pVar;
	AMF0Object *pObject;

	if (!pProperty || pProperty->GetType() != AMF0_OBJECT)
	{
		return false;
	}
	m_strSwfUrl.clear();
	m_nSecureId = 0;
	pObject = AMFCast<AMF0Object>(pProperty);
	pVar = pObject->Get("app");
	if (pVar && pVar->GetType() == AMF_STRING)
	{
		m_strApp = AMFCast<AMFString>(pVar)->GetValue();
		ParseParams();
	}
	pVar = pObject->Get("flashVer");
	if (pVar && pVar->GetType() == AMF_STRING)
	{
		m_strFlashVer = AMFCast<AMFString>(pVar)->GetValue();
	}
	pVar = pObject->Get("swfUrl");
	if (pVar && pVar->GetType() == AMF_STRING)
	{
		m_strSwfUrl = AMFCast<AMFString>(pVar)->GetValue();
	}
	pVar = pObject->Get("tcUrl");
	if (pVar && pVar->GetType() == AMF_STRING)
	{
		m_strTcUrl = AMFCast<AMFString>(pVar)->GetValue();
	}
	pVar = pObject->Get("pageUrl");
	if (pVar && pVar->GetType() == AMF_STRING)
	{
		m_strPageUrl = AMFCast<AMFString>(pVar)->GetValue();
	}
	pVar = pObject->Get("fpad");
	if (pVar && pVar->GetType() == AMF_BOOL)
	{
		m_bFPad = AMFCast<AMFBool>(pVar)->GetValue();
	}
	pVar = pObject->Get("capabilities");
	if (pVar && pVar->GetType() == AMF_NUMBER)
	{
		m_nCapabilities = (long)AMFCast<AMFNumber>(pVar)->GetDoubleValue();
	}
	pVar = pObject->Get("audioCodecs");
	if (pVar && pVar->GetType() == AMF_NUMBER)
	{
		m_nAudioCodecs = (long)AMFCast<AMFNumber>(pVar)->GetDoubleValue();
	}
	pVar = pObject->Get("videoCodecs");
	if (pVar && pVar->GetType() == AMF_NUMBER)
	{
		m_nVideoCodecs = (long)AMFCast<AMFNumber>(pVar)->GetDoubleValue();
	}
	pVar = pObject->Get("videoFunction");
	if (pVar && pVar->GetType() == AMF_NUMBER)
	{
		m_nVideoFunction = (long)AMFCast<AMFNumber>(pVar)->GetDoubleValue();
	}
	pVar = pObject->Get("objectEncoding");
	if (pVar && pVar->GetType() == AMF_NUMBER)
	{
		m_nObjectEncoding = (long)AMFCast<AMFNumber>(pVar)->GetDoubleValue();
	}
	pVar = pObject->Get("secureId");
	if (pVar && pVar->GetType() == AMF_NUMBER)
	{
		m_nSecureId = (uint16_t)AMFCast<AMFNumber>(pVar)->GetDoubleValue();
	}
	pVar = pObject->Get("sdkVer");
	if (pVar && pVar->GetType() == AMF_STRING)
	{
		ClientVersionS sVer;
		BCPString strSdkVer = AMFCast<AMFString>(pVar)->GetValue();
		if (BC_R_SUCCESS == ParseSDKVersion(strSdkVer.c_str()))
		{
			AMFVarPtr pSdkVer(new AMF0Object());
			AMFCast<AMF0Object>(pSdkVer)->PutString("product", m_sSdkVer.product.c_str());
			AMFCast<AMF0Object>(pSdkVer)->PutString("platform", m_sSdkVer.platform.c_str());
			AMFCast<AMF0Object>(pSdkVer)->PutDouble("major", m_sSdkVer.majorVersion);
			AMFCast<AMF0Object>(pSdkVer)->PutDouble("minor", m_sSdkVer.minorVersion);
			AMFCast<AMF0Object>(pSdkVer)->PutDouble("patch", m_sSdkVer.patchVersion);
			AMFCast<AMF0Object>(pSdkVer)->PutDouble("version", m_sSdkVer.version);
			pObject->Put("sdkVersion", pSdkVer);
		}
	}
	return true;
}

bool RTMPUrl::IsInterServerConn() const
{
	return (m_nSecureId == CheckSum(m_strSwfUrl.c_str(), m_strSwfUrl.size() - 1));
}

bool RTMPUrl::ParseUrl(const char *szUrl)
{
	if (!szUrl)
	{
		return false;
	}

	BCPString strAppName;
	BCNetAddress address;
	portNumBits portNum;
	const char *urlSuffix = NULL;

	if (!ParseRTMPUrl(szUrl, &strAppName, &address, &portNum, &urlSuffix))
	{
		return false;
	}
	m_strTcUrl = szUrl;
	int index = m_strTcUrl.find(urlSuffix);
	if (index > 0)
	{
		m_strTcUrl = m_strTcUrl.substr(0, index - 1);
	}

	char szHost[32];
	bc_net_ntop(AF_INET, address.data(), szHost, sizeof(szHost));
	m_strHost = szHost;
	m_eProtocol = 0; // default: RTMP

	// Lower protocol characters
	char szLower[7];
	char *p, *temp;
	const char *pUrl = szUrl;

	for (p = szLower; p < szLower + 6; p++, pUrl++)
	{
		*p = tolower(*pUrl);
	}
	*p = 0;

	// look for usual :// pattern
	p = (LPSTR)strstr(szUrl, "://");
	int len = (int)(p - szUrl);
	if (p == 0)
	{
		return false;
	}

	if (len == 4 && strncmp(szLower, "rtmp", 4) == 0) {
		strncpy(m_szProtocol, szLower, len);
		m_szProtocol[len] = '\0';
		m_eProtocol = RTMP_PROTOCOL_RTMP;
	}
	else if (len == 5 && strncmp(szLower, "rtmpt", 5) == 0) {
		strncpy(m_szProtocol, szLower, len);
		m_szProtocol[len] = '\0';
		m_eProtocol = RTMP_PROTOCOL_RTMPT;
	}
	else if (len == 5 && strncmp(szLower, "rtmps", 5) == 0) {
		strncpy(m_szProtocol, szLower, len);
		m_szProtocol[len] = '\0';
		m_eProtocol = RTMP_PROTOCOL_RTMPS;
	}
	else if (len == 5 && strncmp(szLower, "rtmpe", 5) == 0) {
		strncpy(m_szProtocol, szLower, len);
		m_szProtocol[len] = '\0';
		m_eProtocol = RTMP_PROTOCOL_RTMPE;
	}
	else if (len == 5 && strncmp(szLower, "rtmfp", 5) == 0) {
		strncpy(m_szProtocol, szLower, len);
		m_szProtocol[len] = '\0';
		m_eProtocol = RTMP_PROTOCOL_RTMFP;
	}
	else if (len == 6 && strncmp(szLower, "rtmpte", 6) == 0) {
		strncpy(m_szProtocol, szLower, len);
		m_szProtocol[len] = '\0';
		m_eProtocol = RTMP_PROTOCOL_RTMPTE;
	}
	else
	{
		goto parsehost;
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

	int min = iSlash < iEnd ? iSlash : iEnd+1;
	min = iQues   < min ? iQues   : min;

	int hostlen = iCol < min ? iCol : min;

	//if (min < 256)
	//{
	//	strncpy(m_szHost, p, hostlen);
	//	m_szHost[hostlen] = 0;
	//}
	//else
	//{
	//	LogWarn(_LOCAL_, "Hostname exceeds 255 characters!");
	//}

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
			char portstr[6];
			strncpy(portstr,p,portlen);
			portstr[portlen]=0;

			m_nPort = (uint16_t)atoi(portstr);
			if (m_nPort == 0)
			{
				m_nPort = 1935;
			}
		}
		else
		{
			LogWarn(_LOCAL_, "Port number is longer than 5 characters!");
		}

		p += portlen;
		iEnd -= portlen;
	}

	p++;
	iEnd--;

	// parse application
	//
	// rtmp://host[:port]/app[/appinstance][/...]
	// application = app[/appinstance]
	int iSlash2 = iEnd+1; // 2nd slash
	int iSlash3 = iEnd+1; // 3rd slash

	if ((temp = strstr(p, "/"))!=0)
		iSlash2 = temp-p;

	if ((temp=strstr(p, "?"))!=0)
		iQues = temp-p;

	if (iSlash2 < iEnd)
	{
		if ((temp=strstr(p+iSlash2+1, "/"))!=0)
		{
			iSlash3 = temp - p;
		}
	}

	int nAppLen = iEnd+1; // ondemand, pass all parameters as app
	int nAppNameLen = 8; // ondemand length

	// whatever it is, the '?' and slist= means we need to use
	// everything as app and parse plapath from slist=
	if (iQues < iEnd && strstr(p, "slist="))
	{
		nAppNameLen = iQues;
		nAppLen = iEnd+1; // pass the parameters as well
	}
	else if (strncmp(p, "ondemand/", 9)==0)
	{
		// app = ondemand/foobar, only pass app=ondemand
		nAppLen = 8;
	}
	else   // app!=ondemand, so app is app[/appinstance]
	{
		nAppNameLen = iSlash2 < iEnd ? iSlash2 : iEnd;
		if (iSlash3 < iEnd)
		{
			nAppNameLen = iSlash3;
		}

		nAppLen = nAppNameLen;
	}

	m_strApp.append(p, nAppLen);

	p += nAppNameLen;
	iEnd -= nAppNameLen;

	if (*p == '/')
	{
		p += 1;
		iEnd -= 1;
	}

	ParsePlayPath(p);
	return true;
}

/*
 * Extracts szPlayPath from RTMP URL. szPlayPath is the file part of the
 * URL, i.e. the part that comes after rtmp://host:port/app/
 *
 * Returns the stream name in a format understood by FMS. The name is
 * the szPlayPath part of the URL with formating depending on the stream
 * type:
 *
 * mp4 streams: prepend "mp4:"
 * mp3 streams: prepend "mp3:", remove extension
 * flv streams: remove extension
 */
bool RTMPUrl::ParsePlayPath(const char *szPlayPath)
{
	if (!szPlayPath || !*szPlayPath)
		return false;

	bool bAddMP4 = false;
	bool bAddMP3 = false;
	const char *pTemp;
	const char *pPPStart = szPlayPath;
	int nPPLen = strlen(szPlayPath);

	if ((*pPPStart == '?') && (pTemp=strstr(pPPStart, "slist=")) != 0)
	{
		pPPStart = pTemp+6;
		nPPLen = strlen(pPPStart);

		pTemp = strchr(pPPStart, '&');
		if (pTemp)
		{
			nPPLen = pTemp-pPPStart;
		}
	}

	if (nPPLen >= 4)
	{
		const char *ext = &pPPStart[nPPLen-4];
		if ((strcmp(ext, ".f4v") == 0) || (strcmp(ext, ".mp4") == 0))
		{
			bAddMP4 = 1;
			// Only remove .flv from rtmp URL, not slist params
		}
		else if ((pPPStart == szPlayPath) && (strcmp(ext, ".flv") == 0))
		{
			nPPLen -= 4;
		}
		else if (strcmp(ext, ".mp3") == 0)
		{
			bAddMP3 = 1;
			nPPLen -= 4;
		}
	}

	if (bAddMP4 && (strncmp(pPPStart, "mp4:", 4) != 0))
	{
		m_strPlayPath += "mp4:";
	}
	else if (bAddMP3 && (strncmp(pPPStart, "mp3:", 4) != 0))
	{
		m_strPlayPath += "mp3:";
	}
	m_strPlayPath.append(pPPStart, nPPLen);
	return true;
}

bool RTMPUrl::ParseParams()
{
	const char *p, *s, *szArg = NULL, *szValue = NULL, *szNext = NULL;
	QueryArgPairS sPair;
	std::string strUrl = m_strApp;
	/*
	 * Now, see if there is a ? mark in the URL.  If so, this is
	 * part of the query string, and we will split it from the URL.
	 */
	s = strUrl.c_str() + strUrl.length();
	p = strchr(strUrl.c_str(), '?');
	if (p)
	{
		m_strApp = strUrl.substr(0, p - strUrl.c_str());
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
	return true;
}

BCRESULT RTMPUrl::ParseSDKVersion(LPCSTR lpszVer)
{
	if (!ParseClientVersion(lpszVer, m_sSdkVer))
	{
		return BC_R_INVALIDARG;
	}
	m_strSdkVer = lpszVer;
	std::string strClientOS = m_sSdkVer.product + "-" + m_sSdkVer.platform;
	if (kSDK_IOS.Equal(strClientOS.c_str()))
	{
		m_eClientOS = CLIENT_OS_IOS;
	} 
	else if (kSDK_ANDROID.Equal(strClientOS.c_str()))
	{
		m_eClientOS = CLIENT_OS_ANDROID;
	}
	else if (kSDK_WIN32.Equal(strClientOS.c_str()))
	{
		m_eClientOS = CLIENT_OS_WIN;
	}
	else if (kFMS_WIN.Equal(strClientOS.c_str()))
	{
		m_eClientOS = CLIENT_OS_FMS_WIN;
	}
	else if (kFMS_LINUX.Equal(strClientOS.c_str()))
	{
		m_eClientOS = CLIENT_OS_FMS_LINUX;
	}
	return BC_R_SUCCESS;
}

LPCSTR RTMPUrl::GetParam(LPCSTR szArg, BOOL bCaseSensitive /* = FALSE */)
{
	QueryArgPairS *pPair;

	ASSERT(szArg);

	if (bCaseSensitive)
	{
		for (int32_t i = 0;i < m_arrayArgs.size();i++)
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
		for (int32_t i = 0;i < m_arrayArgs.size();i++)
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

void RTMPUrl::SetConnectInfo(AMFVarWrapper *pProps, AMFVarWrapper *pArgs)
{
	BC_SAFE_DELETE_PTR(m_pDataProp);
	BC_SAFE_DELETE_PTR(m_pArgs);
	m_pDataProp = pProps;
	if (m_pDataProp && m_pDataProp->GetType() == AMF0_OBJECT)
	{
		AMFTable *pTable = (AMFTable *)AMFCast<AMF0Object>(m_pDataProp->var);
		AMFVarPtr pObjectEncoding = pTable->Get("objectEncoding");
		if (pObjectEncoding && pObjectEncoding->GetType() == AMF_NUMBER)
		{
			uint32_t eEncoding = AMFCast<AMFNumber>(
				pObjectEncoding)->GetDoubleValue();
			if (eEncoding == ObjectEncoding::AMF3)
			{
				m_nObjectEncoding = ObjectEncoding::AMF3;
			}
		}
	}
	m_pArgs = pArgs;
}

bool RTMPUrl::ParseRTMPUrl(
	char const* url, 
	BCPString* appname,
	BCNetAddress* address,
	portNumBits* portNum,
	char const** urlSuffix)
{
	do {
		if (!appname && !address && !portNum && !urlSuffix)
		{
			return true;
		}
		// Parse the URL as "rtmp://<server-address-or-name>[:<port>][/<stream-name>]"
		char const* prefix = "rtmp://";
		unsigned const prefixLength = 7;
		portNumBits nPort;
		if (_strncasecmp(url, prefix, prefixLength) != 0) {
			LogError(_LOCAL_, "URL is not of the form \"", prefix, "\"");
			break;
		}

		unsigned const parseBufferSize = 100;
		char parseBuffer[parseBufferSize];
		char const* from = &url[prefixLength];

		// Parse <server-address-or-name>
		char* to = &parseBuffer[0];
		unsigned i;
		for (i = 0; i < parseBufferSize; ++i) {
			if (*from == '\0' || *from == ':' || *from == '/') {
				// We've completed parsing the address
				*to = '\0';
				break;
			}
			*to++ = *from++;
		}
		if (i == parseBufferSize) {
			LogError(_LOCAL_, "URL is too long");
			break;
		}

		if (address)
		{
			BCNetAddressList addresses(parseBuffer);
			if (addresses.numAddresses() == 0) {
				LogError(_LOCAL_, "Failed to find network address for \"",
					parseBuffer, "\"");
				break;
			}
			*address = *(addresses.firstAddress());
		}

		nPort = 1935; // default value
		char nextChar = *from;
		if (nextChar == ':') {
			int portNumInt;
			if (sscanf(++from, "%d", &portNumInt) != 1) {
				LogError(_LOCAL_, "No port number follows ':'");
				break;
			}
			if (portNumInt < 1 || portNumInt > 65535) {
				LogError(_LOCAL_, "Bad port number");
				break;
			}
			nPort = (portNumBits)portNumInt;
			while (*from >= '0' && *from <= '9') ++from; // skip over port number
		}
		if (portNum)
		{
			*portNum = nPort;
		}

		// Parse <application-name>
		if (*from == '/') {
			from++;
		}
		to = &parseBuffer[0];
		for (i = 0; i < parseBufferSize; ++i) {
			if (*from == '\0' || *from == '/') {
				// We've completed parsing the address
				*to = '\0';
				break;
			}
			*to++ = *from++;
		}
		if (i == parseBufferSize) {
			LogError(_LOCAL_, "URL is too long");
			break;
		}

		if (appname)
		{
			char const* appnameStart = &parseBuffer[0];
			unsigned appnameLen = to - appnameStart;
			appname->clear();
			appname->append(appnameStart, appnameLen);
		}

		if (*from == '/') {
			from++;
		}
		// The remainder of the URL is the suffix:
		if (urlSuffix != NULL) *urlSuffix = from;

		return true;
	} while (0);

	return false;
}

///////////////////////////////////////////////////////////////////////////////
// End of namespace : RTMP
///////////////////////////////////////////////////////////////////////////////

} // End of namespace RTMP

///////////////////////////////////////////////////////////////////////////////
// End of file : RTMPUrl.cpp
///////////////////////////////////////////////////////////////////////////////
