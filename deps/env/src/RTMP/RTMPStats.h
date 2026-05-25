
///////////////////////////////////////////////////////////////////////////////
// File : RTMPStats.h
// author : antoniozhou.
///////////////////////////////////////////////////////////////////////////////

#ifndef RTMP_RTMPSTATS_H_INCLUDED__
#define RTMP_RTMPSTATS_H_INCLUDED__

#include <BC/Exports.h>
#include <BC/BCSockAddr.h>
#include <BC/BCNodeList.h>
#include <BC/BCUserData.h>
#include <BC/BCRWLock.h>
#include <BC/BCFixedAlloc.h>
#include <BC/BCFCodec.h>

using namespace BC;

///////////////////////////////////////////////////////////////////////////////
// Namespace : RTMP
///////////////////////////////////////////////////////////////////////////////

namespace RTMP
{

class RTMPStats;

typedef BOOL (*LPFN_NetConnEnumPtr)(RTMPStats &refStats, void *lParam);

///////////////////////////////////////////////////////////////////////////////
// Class : RTMPStats
///////////////////////////////////////////////////////////////////////////////

class RTMP_API RTMPStats 
	: public BCNodeList::Node
	, public BCUserData
{
	DECLARE_FIXED_ALLOC(RTMPStats);
public:
	RTMPStats();
	~RTMPStats();

	BCRESULT		Create();
	void			Reset();

	BCRWLock			m_sStatsLock;
	KBPool				m_sStatsPool;
	uint32_t			m_nStatsSize;
	uint32_t			m_nStatsIndex;
	LPCSTR				m_szClientId;
	LPCSTR				m_szAppName;
	BCSockAddrS			m_sSockLocal;
	BCSockAddrS			m_sSockRemote;
	uint64_t			m_nPacketRecv;
	uint64_t			m_nPacketSend;
	uint64_t			m_nDataRecv;
	uint64_t			m_nDataSend;
	uint32_t			m_tmConnected;
	float32_t			m_fInSpeed;
	float32_t			m_fOutSpeed;
	BCFObject			m_objStats;
	uint64_t			m_nTotalAudioBytesRecv;
	uint64_t			m_nTotalVideoBytesRecv;
	uint64_t			m_nTotalAudioBytesSent;
	uint64_t			m_nTotalVideoBytesSent;

	void			IncTotalAudioBytesRecv(uint64_t nBytes);
	void			IncTotalVideoBytesRecv(uint64_t nBytes);
	void			IncTotalAudioBytesSent(uint64_t nBytes);
	void			IncTotalVideoBytesSent(uint64_t nBytes);
};

typedef TNodeList<RTMPStats>		RTMPStatsList;

///////////////////////////////////////////////////////////////////////////////
// Class : RTMPStatsMgr
///////////////////////////////////////////////////////////////////////////////

class RTMP_API RTMPStatsMgr
{
public:
	RTMPStatsMgr();
	virtual ~RTMPStatsMgr();

	BCRESULT			Create();
	RTMPStats		*	Allocate();
	void				Release(RTMPStats **ppConnStats);
	uint32_t			GetCount();
	BCRESULT			EnumConnStats(
							LPFN_NetConnEnumPtr lpfnEnumFunc, 
							void *lParam);
	virtual BCRESULT	GetStatsById(
							LPCSTR lpClientId,
							LPFN_NetConnEnumPtr lpfnEnumFunc, 
							void *lParam)
	{
		UNUSED(lpClientId);
		UNUSED(lpfnEnumFunc);
		UNUSED(lParam);
		return BC_R_NOTIMPLEMENTED;
	}
	virtual	BCRESULT	PostEvent(
							LPCSTR szClientId,
							uint32_t eCtrlType, 
							uint32_t dwParam1, 
							uint32_t dwParam2 = 0,
							bool bUngetEventBuffer = false,
							LPFN_BCEventProcPtr cbDestroy = NULL)
	{ 
		UNUSED(szClientId);
		UNUSED(eCtrlType);
		UNUSED(dwParam1);
		UNUSED(dwParam2);
		UNUSED(bUngetEventBuffer);
		UNUSED(cbDestroy);
		return BC_R_NOTIMPLEMENTED; 
	}
	virtual	BCRESULT	PostEvent(
							LPCSTR szClientId,
							uint32_t eCtrlType, 
							void *wParam = NULL, 
							void *lParam = NULL,
							bool bUngetEventBuffer = false,
							LPFN_BCEventProcPtr cbDestroy = NULL)
	{
		UNUSED(szClientId);
		UNUSED(eCtrlType);
		UNUSED(wParam);
		UNUSED(lParam);
		UNUSED(bUngetEventBuffer);
		UNUSED(cbDestroy);
		return BC_R_NOTIMPLEMENTED;
	}
protected:
	BCSpinMutex			m_sConnStatsMgrLock;
	RTMPStatsList		m_lstStats;
private:
	DECLARE_NO_COPY_CLASS(RTMPStatsMgr);
};


///////////////////////////////////////////////////////////////////////////////
// End of namespace : RTMP
///////////////////////////////////////////////////////////////////////////////

} // End of namespace RTMP

#endif // RTMP_RTMPSTATS_H_INCLUDED__

///////////////////////////////////////////////////////////////////////////////
// End of file : RTMPStats.h
///////////////////////////////////////////////////////////////////////////////
