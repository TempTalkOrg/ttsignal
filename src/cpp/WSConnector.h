


















































































#ifndef WSCONNECTOR_H_INCLUDED__
#define WSCONNECTOR_H_INCLUDED__

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include <BC/BCFCodec.h>
#include <BC/BCFStream.h>
#include <BC/BCFixedAlloc.h>
#include <BC/BCLog.h>          
#include <BC/BCStream.h>

#include "Interface.h"
#include "SMPacket.h"
#include "TcpChannel.h"
#include "TTErrors.h"
#include "WSParser.h"


using namespace BC;
using namespace SMP;





namespace WS
{

class WSConnector;


struct WSConnectorState;





struct WSUrlParts
{
	std::string	scheme;			
	std::string	host;			
	std::string	hostHeader;		
	uint16_t	port = 0;
	std::string	target;			
	bool		tls = false;
};





class WSConnection 
	: public LLHTTPParser
	, public WSParser
	, public ITcpChannelHandler
{
	DECLARE_FIXED_ALLOC(WSConnection);

	friend class WSConnector;

public:
    
    
    
    
    
    

    class Config
    {
    public:
        Config() {}
        ~Config() {}

		
        BCRESULT		Init(BCFObject* pConfig);

		
		size_t			maxFrameBytes		= WS_DEFAULT_MAX_FRAME_BYTES;
		
		uint64_t		bad_max_frame_bytes	= 0;
		
		uint32_t		connectTimeoutMs	= 10000;
		
		uint32_t		pingIntervalMs		= 0;
		
		uint32_t		idleTimeoutMs		= 0;
		
		
		uint32_t		ifIndex				= 0;
		uint64_t		androidNetHandle	= 0;
		TTVpnPolicy		policy				= TT_VPN_POLICY_UNSET;
		DnsConfig		dns;
		TlsConfig		tls_cfg;
    };

public:
	WSConnection();
	~WSConnection() override;

	
	BCRESULT		Create(
						const std::shared_ptr<WSConnectorState> &state,
						BCFObject *pConfig,
						const Config &defaults,
						uint64_t id,
						IWSConnectionHandler *pHandler);
	
	
	
	BCRESULT		Connect(
						const std::string& url, 
						uint32_t time_out_in_millisec);
	
	
	
	BCRESULT		SetRequestHeader(
						const std::string& name,
						const std::string& value);
	
	
	BCRESULT		SendText(const std::string& text);
	BCRESULT		SendData(LPCVOID data, size_t size);
	
	
	BCRESULT		SendPacket(SMPacketPtr pkt);
	BCRESULT		SendPing();
	
	void			Close(BCRESULT result = BC_R_SUCCESS);
	
	BCRESULT		OpenDumpFile(const char* fileName);
	void			CloseDumpFile();

	IWSConnectionHandler *
					GetHandler() const;
	uint64_t		Id() const { return id_; }
	bool			IsUpgraded() const { return upgraded_.load(); }
	
	std::string		PeerIp() const;
	uint32_t		BoundIfIndex() const;
	std::string		PinMethod() const;
	
	
	
	
	
	
	
	
	
	
	std::string		LastErrorMessage() const;

	
	
	

	
	
	
	static bool		ParseUrl(
						const std::string& url,
						WSUrlParts& out,
						std::string& outErr);
	
	static bool		BuildHandshakeRequest(
						const WSUrlParts& parts,
						const std::string& secKey,
						const HttpHeaderMap& extraHeaders,
						std::string& out,
						std::string& outErr);
	
	
	static bool		CheckHandshakeResponse(
						int status,
						const HttpHeaderMap& headers,
						const std::string& secKey,
						std::string& outErr);

private:
	DECLARE_NO_COPY_CLASS(WSConnection);

	
	void			OnChannelReady() override;
	void			OnChannelData(const void* data, size_t size) override;
	void			OnChannelClosed(
						BCRESULT result,
						const std::string& reason) override;
	
	int				OnWSWrite(
						std::shared_ptr<BCBuffer> data, 
						void* user_data = NULL) override;
	void			OnRecvWSFrame(
						const WSFrameHeader& header, 
						const uint8_t* payload,
						size_t payload_len) override;
	
	int				http_on_url(const char* at, size_t length) override;
	int				http_on_status(const char* at, size_t length) override;
	int				http_on_header_field(const char* at, size_t length) override;
	int				http_on_header_value(const char* at, size_t length) override;
	int				http_on_headers_complete() override;
	int				http_on_message_complete() override;

	
	
	void			ClearHandlerLocked();
	
	
	
	bool			_CancelLocked();
	
	void			_FeedFrames(const void* data, size_t size);
	
	
	bool			_CommitHeader();
	bool			_ChargeHeaderBytes(size_t length);
	bool			_CheckSingleHeaderSize(size_t incoming);
	
	BCRESULT		_SendFrame(uint8_t opcode, BufferPtr payload);
	
	BCRESULT		_SendRawFrame(BufferPtr frame);
	void			_SendPong(const uint8_t* payload, size_t payload_len);
	void			_SendCloseFrame(uint16_t code);
	
	
	void			_Fail(BCRESULT result, const std::string& message);
	void			_DeliverConnectResult(BCRESULT result);
	
	void			_SetLastErrorMessage(const std::string& message);
	void			_DeliverOnce(BCRESULT closeResult, const std::string& closeReason);
	void			_ClassifyClose(BCRESULT result);
	void			_OnActiveCheck();
	void			_TouchNetAction();
	void			_WriteDump(const void* data, size_t size);
	std::string		_CloseReasonText(BCRESULT result, const std::string& raw) const;

private:
	
	std::shared_ptr<WSConnectorState>	state_;
	uint64_t							id_ = 0;
	Config								config_;
	
	IWSConnectionHandler			*	handler_ = NULL;
	void							*	logger_ctx_ = NULL;

	
	
	
	
	
	
	TcpChannel						*	channel_ = NULL;
	
	bool								channel_opened_ = false;
	std::atomic_bool					connect_called_;
	
	uint32_t							timeout_ms_ = 10000;

	
	std::string							url_;
	WSUrlParts							parts_;
	std::string							sec_key_;
	HttpHeaderMap						extra_headers_;
	std::string							req_text_;
	std::atomic_bool					upgraded_;
	int									hs_status_ = 0;
	HttpHeaderMap						hs_headers_;
	size_t								hs_header_bytes_ = 0;
	std::string							cur_field_;
	std::string							cur_value_;
	bool								last_was_value_ = false;

	
	
	BCRESULT							fail_result_ = BC_R_SUCCESS;
	std::string							fail_message_;
	bool								delivered_ = false;
	
	bool								connect_notified_ = false;
	
	bool								connected_ok_ = false;
	bool								peer_closed_frame_ = false;
	uint16_t							close_code_ = 0;
	
	std::atomic_bool					user_closed_;
	std::atomic<BCRESULT>				user_close_result_;
	
	std::atomic_bool					close_frame_sent_;
	
	std::string							peer_ip_;
	uint32_t							bound_ifindex_ = 0;
	std::string							pin_method_;
	
	
	std::string							last_error_message_;

	
	
	int32_t								hs_timer_ = 0;
	int32_t								check_timer_ = 0;
	uint64_t							start_ms_ = 0;
	uint64_t							latest_net_action_ms_ = 0;
	
	
	uint64_t							last_ping_ms_ = 0;
	uint64_t							last_ping_sent_ms_ = 0;

	
	std::atomic<uint64_t>				total_recv_bytes_;
	std::atomic<uint64_t>				total_send_bytes_;
	std::atomic<uint64_t>				total_recv_frames_;
	std::atomic<uint64_t>				total_send_frames_;

	
	std::mutex							dump_lock_;
	std::unique_ptr<BCFOStream>			dump_file_;
};





class WSConnector 
{
	friend class WSConnection;

public:
    
    
    

    class Config
    {
    public:
        Config() {}
        ~Config() {}

        BCRESULT		Init(BCFObject* pConfig);

		
		
		uint32_t				drain_timeout_ms	= 30000;
		uint64_t				bad_drain_timeout	= 0;	
		std::string				log_file;
		int32_t					log_level			= _INFO_;
		
		WSConnection::Config	conn;
		
		std::string				bad_policy_text;
		bool					both_policy_given	= false;
    };

public:
	WSConnector();
	virtual ~WSConnector();

	
	
	
	
	
	
	
	
	
	
	
	
	
	
	
	
	
	
	
	
	BCRESULT				Create(
								BCFObject* pConfig,
								IWSConnectorHandler *pHandler);
	
	
	WSConnPtr				CreateConnection(
								BCFObject *pConfig,
								IWSConnectionHandler* pHandler);
	void				*	GetLoggerCtx();
	
	
	
	
	BCRESULT				OpenLogFile(LPCSTR lpszLogFilePath);
	
	void					Close();
	BCRESULT				GetStats(ConnStatsMap& stats);

	
	TTVpnPolicy				EffectivePolicy() const { return config_.conn.policy; }
	uint32_t				DrainTimeoutMs()  const { return config_.drain_timeout_ms; }
	const DnsConfig		&	DnsSettings()     const { return config_.conn.dns; }
	const TlsConfig		&	TlsSettings()     const { return config_.conn.tls_cfg; }

private:
	DECLARE_NO_COPY_CLASS(WSConnector);

	static void				_LogCallback(void* data, int level, LPCSTR msg);

private:
	
	std::shared_ptr<WSConnectorState>	state_;
    Config								config_;
	void							*	logger_ctx_ = NULL;	
	uint64_t							next_conn_id_ = 0;
	bool								created_ = false;
};





} 

#endif 




