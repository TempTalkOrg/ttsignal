///////////////////////////////////////////////////////////////////////////////
// file : Interface.h
// author : anto
///////////////////////////////////////////////////////////////////////////////
#ifndef INTERFACE_H_INCLUDED__
#define INTERFACE_H_INCLUDED__

#include <map>
#include <string>
#include <memory> // for std::shared_ptr
#include <BC/BCException.h> // for BCException
#include <BC/BCSockAddr.h> // for BCSockAddrS
#include "xquic/xquic.h"
#include "SMPParser.h"

using namespace BC;
using namespace SMP;

#define XQC_MAX_PACKET_LEN              1500

class IRPCStub;
class IServerConnectionHandler;
class SMPServerConnection;
typedef std::shared_ptr<SMPServerConnection>    ServerConnPtr;

typedef	std::map<std::string, size_t>			ConnStatsMap;

///////////////////////////////////////////////////////////////////////////////
// Struct : xqc_quic_lb_ctx_t
///////////////////////////////////////////////////////////////////////////////

typedef struct xqc_quic_lb_ctx_s {
    uint8_t    sid_len = 0;
	uint8_t    sid_buf[XQC_MAX_CID_LEN] = { 0 };
    uint8_t    conf_id = 0;
    uint8_t    cid_len = 0;
	uint8_t    cid_buf[XQC_MAX_CID_LEN] = { 0 };
} xqc_quic_lb_ctx_t;

///////////////////////////////////////////////////////////////////////////////
// Struct : ConnStatS
///////////////////////////////////////////////////////////////////////////////

typedef struct ConnStatS {
    size_t    allocated_conn_size			= 0;
    size_t    active_conn_size				= 0;
	size_t    freed_conn_size				= 0;
	// WSServer specifical used
	size_t	  pending_accept_size			= 0;

	inline std::map<std::string, size_t> ToMap()
	{
		std::map<std::string, size_t> ret;
		ret["allocated_conn_size"] = allocated_conn_size;
		ret["active_conn_size"] = active_conn_size;
		ret["freed_conn_size"] = freed_conn_size;
		return ret;
	}
} ConnStatS;

///////////////////////////////////////////////////////////////////////////////
// Class : IRPCStub
///////////////////////////////////////////////////////////////////////////////

typedef void (*IRPCStubDtor)(IRPCStub &);
typedef IRPCStubDtor		LPFN_IRPCStubDtor;

class IRPCStub
{
public:
	IRPCStub(uint32_t nTransId) 
		: m_nTransId(nTransId)
		, m_lpfnDtor(NULL)
		, m_result(BC_R_SUCCESS)
	{
		memzero(m_szCmd, sizeof(m_szCmd));
		memzero(m_lParams, sizeof(m_lParams));
	}
	IRPCStub(const IRPCStub& other)
	{
		m_nTransId = other.m_nTransId;
		m_lpfnDtor = other.m_lpfnDtor;
		m_result = other.m_result;
		memcpy(m_szCmd, other.m_szCmd, sizeof(m_szCmd));
		memcpy(m_lParams, other.m_lParams, sizeof(m_lParams));
	}
	virtual ~IRPCStub()
	{
		//if (m_lpfnDtor)
		//{
		//	(m_lpfnDtor)(*this);
		//}		
	}
	virtual IRPCStub *Clone(){
		return new IRPCStub(*this);
	}
	void	SetCmdName(LPCSTR lpszCmdName) {
		strncpy(m_szCmd, lpszCmdName, sizeof(m_szCmd));
	}

	uint32_t				m_nTransId;
	LPFN_IRPCStubDtor		m_lpfnDtor;
	BCRESULT				m_result;
	char					m_szCmd[MAX_PATH];
	uint64_t				m_lParams[10];
	KBPool					m_sPool;
};

///////////////////////////////////////////////////////////////////////////////
// Class : RecvInfo
///////////////////////////////////////////////////////////////////////////////

class RecvInfo
{
	DECLARE_FIXED_ALLOC(RecvInfo);
public:
	RecvInfo() : size(0), recv_time(0), sr_process(0) {
		memset(&scid, 0, sizeof(scid));
		memset(&dcid, 0, sizeof(dcid));
		memset(&local_addr, 0, sizeof(local_addr));
		memset(&peer_addr, 0, sizeof(peer_addr));
	}
	virtual ~RecvInfo(){}

	uint8_t			data[XQC_MAX_PACKET_LEN] = { 0 };
	size_t			size;
	xqc_cid_t		scid;
	xqc_cid_t		dcid;
	BCSockAddrS		local_addr;
	BCSockAddrS		peer_addr;
	xqc_msec_t		recv_time;
	xqc_bool_t		sr_process;
};

///////////////////////////////////////////////////////////////////////////////
// Class : IConnectorHandler
///////////////////////////////////////////////////////////////////////////////

class IConnectorHandler
{
public:
	IConnectorHandler(){}
	virtual ~IConnectorHandler(){}

	virtual void		OnExecDone(IRPCStub *pStub)				= 0;
	virtual void		OnLog(int level, LPCSTR lpszMsg)		= 0;
	virtual void		OnClosed()								= 0;
	virtual void		OnException(BCException &)				= 0;
private:
	DECLARE_NO_COPY_CLASS(IConnectorHandler);
};

///////////////////////////////////////////////////////////////////////////////
// Class : IConnectionHandler
///////////////////////////////////////////////////////////////////////////////

class IConnectionHandler
{
public:
	IConnectionHandler(){}
	virtual ~IConnectionHandler(){}

	virtual void		OnHandshakeFinished()				= 0;
	virtual void		OnExecDone(IRPCStub *pStub)			= 0;
	virtual void 		OnStreamCreated(uint32_t nStreamId) = 0;
	virtual void		OnStreamClosed(uint32_t nStreamId)	= 0;
	virtual void		OnStreamDataAcked(
							uint32_t nStreamId,
							xqc_usec_t ack_delay_time,
							size_t acked_bytes,
							size_t inflight_bytes)			= 0;
	virtual void		OnStreamDataSent(
							uint32_t nStreamId, 
							uint32_t nTransId,
							size_t size)					= 0;
	virtual void		OnRecvCmd(
							const SMPHeader &refHeader,
							const char* lpszCmd, 
							size_t size)					= 0;
	virtual void		OnRecvData(
							const SMPHeader &refHeader,
							LPCVOID data,
							size_t size)					= 0;
	virtual void		OnRestart(
							BCRESULT result,
							const char *lpszAddr)			= 0;
	virtual void		OnClosed(LPCSTR strReason)			= 0;
	virtual void		OnException(BCException &)			= 0;
private:
	DECLARE_NO_COPY_CLASS(IConnectionHandler);
};

///////////////////////////////////////////////////////////////////////////////
// Class : IServerHandler
///////////////////////////////////////////////////////////////////////////////

class IServerHandler
{
public:
	IServerHandler(){}
	virtual ~IServerHandler(){}

	virtual void		OnNewConn(RecvInfo *pInfo)				= 0;
	virtual void		OnAccept(IServerConnectionHandler *h)	= 0;
	virtual void		OnExecDone(IRPCStub *pStub)				= 0;
	virtual void		OnClosed()								= 0;
	virtual void		OnException(BCException &)				= 0;
private:
	DECLARE_NO_COPY_CLASS(IServerHandler);
};

///////////////////////////////////////////////////////////////////////////////
// Class : IServerConnectionHandler
///////////////////////////////////////////////////////////////////////////////

class IServerConnectionHandler
{
public:
	IServerConnectionHandler(){}
	virtual ~IServerConnectionHandler(){}

	virtual void		SetConnection(ServerConnPtr pConn)				= 0;
	virtual void		OnHandshakeFinished()							= 0;
	virtual void		OnExecDone(IRPCStub *pStub)						= 0;
	virtual void		OnConnect(const char* lpszCmd, size_t size)		= 0;
	virtual void		OnRecvCmd(
							const SMPHeader &refHeader,
							const char* lpszCmd, 
							size_t size)								= 0;
	virtual void		OnRecvData(
							const SMPHeader &refHeader,
							LPCVOID data,
							size_t size)								= 0;
	virtual void		OnClosed(LPCSTR strReason)						= 0;
	virtual void		OnException(BCException &)						= 0;
private:
	DECLARE_NO_COPY_CLASS(IServerConnectionHandler);
};


///////////////////////////////////////////////////////////////////////////////
// class : ISMPServerConnListener
///////////////////////////////////////////////////////////////////////////////

class ISMPServerConnListener
{
public:
	ISMPServerConnListener() {}
	virtual ~ISMPServerConnListener() {}

	virtual void	OnSendFailed(ServerConnPtr conn, size_t size)		= 0;
	virtual void	OnConnClosed(ServerConnPtr conn)					= 0;
private:
	DECLARE_NO_COPY_CLASS(ISMPServerConnListener);
};

#endif // INTERFACE_H_INCLUDED__

///////////////////////////////////////////////////////////////////////////////
// End of file : Interface.h
///////////////////////////////////////////////////////////////////////////////
