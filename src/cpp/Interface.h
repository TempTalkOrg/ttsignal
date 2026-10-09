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

// WebSocket 那一侧的连接。jmp 把这个 typedef 放在 Interface.h 里（Interface.h:32），
// 本项目照做 —— 绑定层拿到的是 WSConnPtr，不该为此 include 整个 WSConnector.h。
namespace WS
{
	class WSConnection;
}
typedef std::shared_ptr<WS::WSConnection>		WSConnPtr;

// 与 LLHTTPParser.h:32 的定义逐字一致 —— 同一类型的重复 typedef 在 C++ 里合法
// （TcpChannel.h 顶部的 BufferPtr 是同样的处理）。
//
// ⚠️ 刻意**不** include LLHTTPParser.h：那会把 llhttp.h 拖进每一个 include
// Interface.h 的 TU，而 src/cpp/http-parser/http_parser.h（napi 的
// JsSMPConnectorWrap.cpp 还在用）与 llhttp.h 的 HTTP_* 枚举名逐个撞车，
// 编译期直接报 "redefinition of enumerator 'HTTP_DELETE'" 一整屏。
typedef std::map<std::string, std::string>		HttpHeaderMap;

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
// class : IWSConnectorHandler
//
// WebSocket 连接器级回调。与 IWSConnectionHandler 一起沿用 jmp 的接口位置
// （都放在 Interface.h 里），下一轮 JNI / NAPI / Swift 绑定按这里对齐。
//
// ⚠️ OnLog 的限制远比其它回调严格：它是被 BC 日志器持着一把**全局非递归自旋锁**
//    调进来的，因此在 OnLog 里**不得**调用 WSConnector / WSConnection 任何会打
//    日志的接口（Create / CreateConnection / Connect / 析构），否则重进那把锁
//    就是 100% CPU 空转挂死。细节见 WSConnector.h 顶部契约。
///////////////////////////////////////////////////////////////////////////////

class IWSConnectorHandler
{
public:
	IWSConnectorHandler() {}
	virtual ~IWSConnectorHandler() {}

	virtual void		OnLog(int level, LPCSTR lpszMsg)	= 0;
	virtual void		OnClosed()							= 0;
	virtual void		OnException(BCException &)			= 0;
private:
	DECLARE_NO_COPY_CLASS(IWSConnectorHandler);
};

///////////////////////////////////////////////////////////////////////////////
// Class : IWSConnectionHandler
//
// 一条 WebSocket 连接的回调。时序契约（详见 WSConnector.h）：
//   * Connect() 返回 BC_R_SUCCESS 之后，OnConnectResult 恰好回调一次；
//   * result == BC_R_SUCCESS 时，之后还会恰好回调一次 OnClosed；
//   * result != BC_R_SUCCESS 时，**不会**再有 OnClosed —— 握手就没成功，
//     没有"连接关闭"这件事可报。
//
// ⚠️ OnRecvText 拿到的是 NUL 结尾的 C 字符串（签名与 jmp 一致，便于绑定层直接
//    NewStringUTF / napi_create_string_utf8）。因此**文本帧里内嵌的 NUL 会造成
//    截断**；需要精确长度的二进制内容请走 binary 帧（OnRecvData 带 size）。
///////////////////////////////////////////////////////////////////////////////

class IWSConnectionHandler
{
public:
	IWSConnectionHandler(){}
	virtual ~IWSConnectionHandler(){}

	virtual void		OnConnectResult(
							BCRESULT result,
							const HttpHeaderMap &headers)	= 0;
	virtual void		OnRecvText(LPCSTR lpszText)			= 0;
	virtual void		OnRecvData(LPCVOID data, size_t size)= 0;
	virtual void		OnClosed(LPCSTR lpszReason)			= 0;
	virtual void		OnException(BCException &)			= 0;
private:
	DECLARE_NO_COPY_CLASS(IWSConnectionHandler);
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
