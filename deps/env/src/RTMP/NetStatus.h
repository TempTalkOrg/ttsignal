///////////////////////////////////////////////////////////////////////////////
// file : NetStatus.h
// author : antoniozhou
///////////////////////////////////////////////////////////////////////////////


#ifndef RTMP_NETSTATUS_H_INCLUDED__
#define RTMP_NETSTATUS_H_INCLUDED__

#include <RTMP/Exports.h>
#include <RTMP/Packet.h>


///////////////////////////////////////////////////////////////////////////////
// Namespace : StatusCode
///////////////////////////////////////////////////////////////////////////////

namespace StatusCode
{
	RTMPDLLEXPORT_DATA(extern const char *)S_NS_PLAYSTART;
	RTMPDLLEXPORT_DATA(extern const char *)S_NS_PLAYSTOP;
	RTMPDLLEXPORT_DATA(extern const char *)S_NS_PLAYRESET;
	RTMPDLLEXPORT_DATA(extern const char *)S_NS_PLAYPUBLISHNOTIFY;
	RTMPDLLEXPORT_DATA(extern const char *)S_NS_PLAYUNPUBLISHNOTIFY;
	RTMPDLLEXPORT_DATA(extern const char *)S_NS_PLAYSWITCH;
	RTMPDLLEXPORT_DATA(extern const char *)S_NS_PLAYCOMPLETE;
	RTMPDLLEXPORT_DATA(extern const char *)S_NS_PLAYTRANSITIONCOMPLETE;
	RTMPDLLEXPORT_DATA(extern const char *)S_NS_PUBLISHSTART;
	RTMPDLLEXPORT_DATA(extern const char *)S_NS_PUBLISHIDLE;
	RTMPDLLEXPORT_DATA(extern const char *)S_NS_UNPUBLISHSUCCESS;
	RTMPDLLEXPORT_DATA(extern const char *)S_NS_SEEKNOTIFY;
	RTMPDLLEXPORT_DATA(extern const char *)S_NS_RECORDSTART;
	RTMPDLLEXPORT_DATA(extern const char *)S_NS_RECORDSTOP;
	RTMPDLLEXPORT_DATA(extern const char *)S_NS_PAUSENOTIFY;
	RTMPDLLEXPORT_DATA(extern const char *)S_NS_UNPAUSENOTIFY;
	RTMPDLLEXPORT_DATA(extern const char *)S_SO_FLUSHSUCCESS;
	RTMPDLLEXPORT_DATA(extern const char *)S_NS_BUFFEREMPTY;
	RTMPDLLEXPORT_DATA(extern const char *)S_NS_BUFFERFULL;
	RTMPDLLEXPORT_DATA(extern const char *)S_NS_BUFFERFLUSH;
	RTMPDLLEXPORT_DATA(extern const char *)S_NS_CONNECTSUCCESS;
	RTMPDLLEXPORT_DATA(extern const char *)S_NS_CONNECTCLOSED;
	RTMPDLLEXPORT_DATA(extern const char *)E_NC_CONNECTFAILED;
	RTMPDLLEXPORT_DATA(extern const char *)E_NC_CONNECTREJECTED;
	RTMPDLLEXPORT_DATA(extern const char *)E_NC_CONNECTAPPSHUTDOWN;
	RTMPDLLEXPORT_DATA(extern const char *)E_NC_CONNECTINVALIDAPP;
	RTMPDLLEXPORT_DATA(extern const char *)E_NC_CALLBADVERSION;
	RTMPDLLEXPORT_DATA(extern const char *)E_NC_CALLFAILED;
	RTMPDLLEXPORT_DATA(extern const char *)E_NC_CALLPROHIBITED;
	RTMPDLLEXPORT_DATA(extern const char *)E_NS_CREATESTREAMFAILED;
	RTMPDLLEXPORT_DATA(extern const char *)E_NS_PLAYFAILED;
	RTMPDLLEXPORT_DATA(extern const char *)E_NS_PLAYSTREAMNOTFOUND;
	RTMPDLLEXPORT_DATA(extern const char *)E_NS_PLAYINSUFFICIENTBW;
	RTMPDLLEXPORT_DATA(extern const char *)E_NS_PLAYFILESTRUCTUREINVALID;
	RTMPDLLEXPORT_DATA(extern const char *)E_NS_PLAYNOSUPPORTEDTRACKFOUND;
	RTMPDLLEXPORT_DATA(extern const char *)E_NS_PUBLISHBADNAME;
	RTMPDLLEXPORT_DATA(extern const char *)E_NS_SEEKFAILED;
	RTMPDLLEXPORT_DATA(extern const char *)E_NS_SEEKINVALIDTIME;
	RTMPDLLEXPORT_DATA(extern const char *)E_NS_RECORDNOACCESS;
	RTMPDLLEXPORT_DATA(extern const char *)E_NS_RECORDFAILED;
	RTMPDLLEXPORT_DATA(extern const char *)E_NS_PAUSEFAILED;
	RTMPDLLEXPORT_DATA(extern const char *)E_NS_CONNECTFAILED;
	RTMPDLLEXPORT_DATA(extern const char *)E_NS_CONNECTREJECTED;
	RTMPDLLEXPORT_DATA(extern const char *)E_SO_FLUSHFAILED;
	RTMPDLLEXPORT_DATA(extern const char *)E_SO_BADPERSISTENCE;
	RTMPDLLEXPORT_DATA(extern const char *)E_SO_URIMISMATCH;
	RTMPDLLEXPORT_DATA(extern const char *)E_SO_NOREADACCESS;
	RTMPDLLEXPORT_DATA(extern const char *)E_SO_NOWRITEACCESS;
	RTMPDLLEXPORT_DATA(extern const char *)E_SO_OBJECTCREATIONFAILED;
	RTMPDLLEXPORT_DATA(extern const char *)R_NC_CONNECTSUCCESS;
	RTMPDLLEXPORT_DATA(extern const char *)R_NC_CONNECTCLOSED;
	RTMPDLLEXPORT_DATA(extern const char *)R_NS_CREATESTREAMSUCCESS;
	RTMPDLLEXPORT_DATA(extern const char *)R_NS_RELEASESTREAMSUCCESS;
	RTMPDLLEXPORT_DATA(extern const char *)E_NG_CONNECTFAILED;
	RTMPDLLEXPORT_DATA(extern const char *)E_NG_CONNECTREJECTED;
	RTMPDLLEXPORT_DATA(extern const char *)R_NG_CONNECTSUCCESS;
	RTMPDLLEXPORT_DATA(extern const char *)R_NG_LOCALCOVERAGENOTIFY;
	RTMPDLLEXPORT_DATA(extern const char *)R_NG_MULTICASTSTREAMPUBLISHNOTIFY;
	RTMPDLLEXPORT_DATA(extern const char *)R_NG_MULTICASTSTREAMUNPUBLISHNOTIFY;
	RTMPDLLEXPORT_DATA(extern const char *)R_NG_NEIGHBORCONNECT;
	RTMPDLLEXPORT_DATA(extern const char *)R_NG_NEIGHBORDISCONNECT;
	RTMPDLLEXPORT_DATA(extern const char *)R_NG_POSTINGNOTIFY;
	RTMPDLLEXPORT_DATA(extern const char *)R_NG_REPLICATIONFETCHFAILED;
	RTMPDLLEXPORT_DATA(extern const char *)R_NG_REPLICATIONFETCHRESULT;
	RTMPDLLEXPORT_DATA(extern const char *)R_NG_REPLICATIONFETCHSENDNOTIFY;
	RTMPDLLEXPORT_DATA(extern const char *)R_NG_REPLICATIONREQUEST;
	RTMPDLLEXPORT_DATA(extern const char *)R_NG_SENDTONOTIFY;
	RTMPDLLEXPORT_DATA(extern const char *)R_NC_CALLSUCCESS;

///////////////////////////////////////////////////////////////////////////////
// End of namespace :
///////////////////////////////////////////////////////////////////////////////
}

typedef enum RtmpStatusE
{
	RTMP_S_NS_PLAYSTART							= 0,
	RTMP_S_NS_PLAYSTOP							= 1,
	RTMP_S_NS_PLAYRESET							= 2,
	RTMP_S_NS_PLAYPUBLISHNOTIFY					= 3,
	RTMP_S_NS_PLAYUNPUBLISHNOTIFY				= 4,
	RTMP_S_NS_PLAYSWITCH						= 5,
	RTMP_S_NS_PLAYCOMPLETE						= 6,
	RTMP_S_NS_PLAYTRANSITIONCOMPLETE			= 7,
	RTMP_S_NS_PUBLISHSTART						= 8,
	RTMP_S_NS_PUBLISHIDLE						= 9,
	RTMP_S_NS_UNPUBLISHSUCCESS					= 10,
	RTMP_S_NS_SEEKNOTIFY						= 11,
	RTMP_S_NS_RECORDSTART						= 12,
	RTMP_S_NS_RECORDSTOP						= 13,
	RTMP_S_NS_PAUSENOTIFY						= 14,
	RTMP_S_NS_UNPAUSENOTIFY						= 15,
	RTMP_S_SO_FLUSHSUCCESS						= 16,
	RTMP_S_NS_BUFFEREMPTY						= 17,
	RTMP_S_NS_BUFFERFULL						= 18,
	RTMP_S_NS_BUFFERFLUSH						= 19,
	RTMP_S_NS_CONNECTSUCCESS					= 20,
	RTMP_S_NS_CONNECTCLOSED						= 21,
	RTMP_S_NG_CONNECTSUCCESS					= 22,
	RTMP_R_NG_LOCALCOVERAGENOTIFY				= 23,
	RTMP_R_NG_MULTICASTSTREAMPUBLISHNOTIFY		= 24,
	RTMP_R_NG_MULTICASTSTREAMUNPUBLISHNOTIFY	= 25,
	RTMP_R_NG_NEIGHBORCONNECT					= 26,
	RTMP_R_NG_NEIGHBORDISCONNECT				= 27,
	RTMP_R_NG_POSTINGNOTIFY						= 28,
	RTMP_R_NG_REPLICATIONFETCHFAILED			= 29,
	RTMP_R_NG_REPLICATIONFETCHRESULT			= 30,
	RTMP_R_NG_REPLICATIONFETCHSENDNOTIFY		= 31,
	RTMP_R_NG_REPLICATIONREQUEST				= 32,
	RTMP_R_NG_SENDTONOTIFY						= 33,
}RtmpStatusE;

typedef enum RtmpErrorE
{
	RTMP_E_NOERROR								= 50,
	RTMP_E_NC_CONNECTFAILED						= 51,
	RTMP_E_NC_CONNECTREJECTED					= 52,
	RTMP_E_NC_CONNECTAPPSHUTDOWN				= 53,
	RTMP_E_NC_CONNECTINVALIDAPP					= 54,
	RTMP_E_NC_CALLBADVERSION					= 55,
	RTMP_E_NC_CALLFAILED						= 56,
	RTMP_E_NC_CALLPROHIBITED					= 57,
	RTMP_E_NS_CREATESTREAMFAILED				= 58,
	RTMP_E_NS_PLAYFAILED						= 59,
	RTMP_E_NS_PLAYSTREAMNOTFOUND				= 60,
	RTMP_E_NS_PLAYINSUFFICIENTBW				= 61,
	RTMP_E_NS_PLAYFILESTRUCTUREINVALID			= 62,
	RTMP_E_NS_PLAYNOSUPPORTEDTRACKFOUND			= 63,
	RTMP_E_NS_PUBLISHBADNAME					= 64,
	RTMP_E_NS_SEEKFAILED						= 65,
	RTMP_E_NS_SEEKINVALIDTIME					= 66,
	RTMP_E_NS_RECORDNOACCESS					= 67,
	RTMP_E_NS_RECORDFAILED						= 68,
	RTMP_E_NS_PAUSEFAILED						= 69,
	RTMP_E_NS_CONNECTFAILED						= 70,
	RTMP_E_NS_CONNECTREJECTED					= 71,
	RTMP_E_SO_FLUSHFAILED						= 72,
	RTMP_E_SO_BADPERSISTENCE					= 73,
	RTMP_E_SO_URIMISMATCH						= 74,
	RTMP_E_SO_NOREADACCESS						= 75,
	RTMP_E_SO_NOWRITEACCESS						= 76,
	RTMP_E_SO_OBJECTCREATIONFAILED				= 77,
	RTMP_E_NG_CONNECTFAILED						= 78,
	RTMP_E_NG_CONNECTREJECTED					= 79,
}RtmpErrorE;

typedef enum RtmpResultE
{
	RTMP_R_NC_CONNECTSUCCESS					= 90,
	RTMP_R_NC_CONNECTCLOSED						= 91,
	RTMP_R_NS_CREATESTREAMSUCCESS				= 92,
}RtmpResultE;

//////////////////////////////////////////////////////////////////////////
/// namespace RTMP
//////////////////////////////////////////////////////////////////////////
namespace RTMP
{

//////////////////////////////////////////////////////////////////////////
/// class MRConnect
//////////////////////////////////////////////////////////////////////////

class RTMP_API MRConnect : public MCommand
{
	DECLARE_FIXED_ALLOC(MRConnect);
public:
	typedef enum ConnectStatusE
	{
		CONNECT_SUCCESS					= 1,
		CONNECT_CLOSED					= 2,
		CONNECT_FAILED					= 3,
		CONNECT_REJECTED				= 4,
		CONNECT_APPSHUTDOWN				= 5,
		CONNECT_INVALIDAPP				= 6,
	}ConnectStatusE;
public:
	MRConnect(
		uint32_t eStatus,
		uint32_t nChannelId,
		uint32_t eEncoding = ObjectEncoding::AMF0);
	virtual ~MRConnect();

private:
	DECLARE_NO_COPY_CLASS(MRConnect);
	uint32_t				m_eStatusType;
};

//////////////////////////////////////////////////////////////////////////
/// class MRStream
//////////////////////////////////////////////////////////////////////////

class RTMP_API MRStream : public MCommand
{
	DECLARE_FIXED_ALLOC(MRStream);
public:
	explicit MRStream(uint32_t eStatus);
	virtual ~MRStream();

private:
	DECLARE_NO_COPY_CLASS(MRStream);
	uint32_t				m_eStatusType;
};

//////////////////////////////////////////////////////////////////////////
/// end of namespace RTMP
//////////////////////////////////////////////////////////////////////////

} // End of namespace RTMP

#endif // RTMP_NETSTATUS_H_INCLUDED__

///////////////////////////////////////////////////////////////////////////////
// End of file : NetStatus.h
///////////////////////////////////////////////////////////////////////////////
