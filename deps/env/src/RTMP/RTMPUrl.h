///////////////////////////////////////////////////////////////////////////////
// file : RTMPUrl.h
// author : antoniozhou
///////////////////////////////////////////////////////////////////////////////


#ifndef RTMP_RTMPURL_H_INCLUDED__
#define RTMP_RTMPURL_H_INCLUDED__

#include <RTMP/Exports.h>
#include <RTMP/AMF.h>
#include <BC/BCMemPool.h>
#include <BC/BCNetAddress.h>

using namespace AMF;
using namespace AMFType;

///////////////////////////////////////////////////////////////////////////////
// Namespace : RTMP
///////////////////////////////////////////////////////////////////////////////

namespace RTMP
{


typedef enum RtmpProtocolE
{
	RTMP_PROTOCOL_UNDEFINED		= -1,
	RTMP_PROTOCOL_RTMP			=  0,
	RTMP_PROTOCOL_RTMPT			=  1, // not yet supported
	RTMP_PROTOCOL_RTMPS			=  2, // not yet supported
	RTMP_PROTOCOL_RTMPE			=  3, // not yet supported
	RTMP_PROTOCOL_RTMPTE		=  4, // not yet supported
	RTMP_PROTOCOL_RTMFP			=  5  // not yet supported
}RtmpProtocolE;

typedef enum ClientOS
{
	CLIENT_OS_UNKNOWN		= 0,
	CLIENT_OS_WIN			= 1,
	CLIENT_OS_MAC			= 2,
	CLIENT_OS_ANDROID		= 3,
	CLIENT_OS_IOS			= 4,
	CLIENT_OS_FMS_WIN		= 5,
	CLIENT_OS_FMS_LINUX		= 6,
}ClientOS;

typedef struct ClientVersionS
{
	std::string			product;
	std::string			platform;
	uint16_t			majorVersion;
	uint16_t			minorVersion;
	uint16_t			patchVersion;
	uint32_t			version;
	ClientVersionS() : majorVersion(0), minorVersion(0), patchVersion(0), version(0){}
}ClientVersionS;

///////////////////////////////////////////////////////////////////////////////
// class : RTMPUrl
///////////////////////////////////////////////////////////////////////////////

class RTMP_API RTMPUrl
{
	typedef struct QueryArgPairS
	{
		LPCSTR		szArg;
		LPCSTR		szValue;

		QueryArgPairS() : szArg(NULL), szValue(NULL) {}
		~QueryArgPairS(){};
	}QueryArgPairS;
public:
	RTMPUrl();
	~RTMPUrl();

	static uint16_t	CheckSum(LPCVOID lpData, uint32_t nSize);
	bool			ParseUrl(const char *szUrl);
	LPCSTR			GetParam(LPCSTR szArg, BOOL bCaseSensitive = FALSE);
	bool			DecodeUrl(AMFVarPtr pProperty);
	bool			IsInterServerConn() const;
	bool			ParsePlayPath(const char *szPlayPath);
	void			SetConnectInfo(AMFVarWrapper *pProps, AMFVarWrapper *pArgs);
	static bool		ParseRTMPUrl(
						char const* url, 
						BCPString* appname,
						BCNetAddress* address, 
						portNumBits* portNum, 
						char const** urlSuffix);

	// NetConnection proterties
	std::string			m_strApp;
	std::string			m_strFlashVer;
	std::string			m_strSwfUrl;
	std::string			m_strTcUrl;
	bool				m_bFPad;
	long				m_nCapabilities;
	long				m_nAudioCodecs;
	long				m_nVideoCodecs;
	long				m_nVideoFunction;
	std::string			m_strPageUrl;
	uint32_t			m_nObjectEncoding;
	uint16_t			m_nSecureId;
	// After parse
	int32_t				m_eProtocol;
	std::string			m_strHost;
	char				m_szProtocol[7];
	uint16_t			m_nPort;
	std::string			m_strPlayPath;
	std::string			m_strSdkVer;
	ClientOS			m_eClientOS;
	ClientVersionS		m_sSdkVer;
	// Data property object
	AMFVarWrapper	*	m_pDataProp;
	AMFVarWrapper	*	m_pArgs;
protected:
	bool			ParseParams();
	BCRESULT		ParseSDKVersion(LPCSTR lpszVer);
private:
	DECLARE_NO_COPY_CLASS(RTMPUrl);
	typedef BCVector<QueryArgPairS>	ArgArray;
	ArgArray			m_arrayArgs;
	KBPool				m_sPool;
};


///////////////////////////////////////////////////////////////////////////////
// End of namespace : RTMP
///////////////////////////////////////////////////////////////////////////////

} // End of namespace RTMP

#endif // RTMP_RTMPURL_H_INCLUDED__

///////////////////////////////////////////////////////////////////////////////
// End of file : RTMPUrl.h
///////////////////////////////////////////////////////////////////////////////
