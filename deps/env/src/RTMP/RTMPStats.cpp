
///////////////////////////////////////////////////////////////////////////////
// File : RTMPStats.cpp
// author : antoniozhou.
///////////////////////////////////////////////////////////////////////////////

#include "Precompile.h"
#include <RTMP/RTMPStats.h>

///////////////////////////////////////////////////////////////////////////////
// Namespace : RTMP
///////////////////////////////////////////////////////////////////////////////

namespace RTMP
{

///////////////////////////////////////////////////////////////////////////////
// Class : RTMPStats
///////////////////////////////////////////////////////////////////////////////

IMPLEMENT_FIXED_ALLOC(RTMPStats, 64);

RTMPStats::RTMPStats() 
	: m_nStatsSize(0), m_nStatsIndex(0), m_szClientId(NULLPSTRING)
	, m_szAppName(NULLPSTRING), m_nPacketRecv(0), m_nPacketSend(0)
	, m_nDataRecv(0), m_nDataSend(0), m_tmConnected(0), m_fInSpeed(0.0)
	, m_fOutSpeed(0.0), m_nTotalAudioBytesRecv(0), m_nTotalVideoBytesRecv(0)
	, m_nTotalAudioBytesSent(0), m_nTotalVideoBytesSent(0)
{
	memzero(&m_sSockLocal, sizeof(m_sSockLocal));
	memzero(&m_sSockRemote, sizeof(m_sSockRemote));
}

RTMPStats::~RTMPStats()
{
	m_sStatsLock.Destroy();
}

BCRESULT RTMPStats::Create()
{
	BCRESULT result;

	result = m_sStatsLock.Create(0, 1);
	if (result != BC_R_SUCCESS)
	{
		goto return_error;
	}

	return BC_R_SUCCESS;

return_error:
	return result;
}

void RTMPStats::Reset()
{
	m_sStatsLock.Lock(bc_rwlocktype_write);
	m_sStatsPool.Clear();
	m_nStatsSize = 0;
	m_nStatsIndex = 0;
	m_szClientId = NULLPSTRING;
	m_szAppName = NULLPSTRING;
	memzero(&m_sSockLocal, sizeof(m_sSockLocal));
	memzero(&m_sSockRemote, sizeof(m_sSockRemote));
	m_nPacketRecv = 0;
	m_nPacketSend = 0;
	m_nDataRecv = 0;
	m_nDataSend = 0;
	m_tmConnected = 0;
	m_nTotalAudioBytesRecv = 0;
	m_nTotalVideoBytesRecv = 0;
	m_nTotalAudioBytesSent = 0;
	m_nTotalVideoBytesSent = 0;
	m_objStats.Clear();
	m_sStatsLock.Unlock(bc_rwlocktype_write);
}

void RTMPStats::IncTotalAudioBytesRecv(uint64_t nBytes)
{
	m_sStatsLock.Lock(bc_rwlocktype_write);
	m_nTotalAudioBytesRecv += nBytes;
	m_sStatsLock.Unlock(bc_rwlocktype_write);
}

void RTMPStats::IncTotalVideoBytesRecv(uint64_t nBytes)
{
	m_sStatsLock.Lock(bc_rwlocktype_write);
	m_nTotalVideoBytesRecv += nBytes;
	m_sStatsLock.Unlock(bc_rwlocktype_write);
}

void RTMPStats::IncTotalAudioBytesSent(uint64_t nBytes)
{
	m_sStatsLock.Lock(bc_rwlocktype_write);
	m_nTotalAudioBytesSent += nBytes;
	m_sStatsLock.Unlock(bc_rwlocktype_write);
}

void RTMPStats::IncTotalVideoBytesSent(uint64_t nBytes)
{
	m_sStatsLock.Lock(bc_rwlocktype_write);
	m_nTotalVideoBytesSent += nBytes;
	m_sStatsLock.Unlock(bc_rwlocktype_write);
}

///////////////////////////////////////////////////////////////////////////////
// Class : RTMPStatsMgr
///////////////////////////////////////////////////////////////////////////////

RTMPStatsMgr::RTMPStatsMgr()
{
	//
}

RTMPStatsMgr::~RTMPStatsMgr()
{
	//
}

BCRESULT RTMPStatsMgr::Create()
{
	return BC_R_SUCCESS;
}

RTMPStats *RTMPStatsMgr::Allocate()
{
	RTMPStats *pStats;
	
	pStats = new RTMPStats();
	if (pStats)
	{
		BCSpinMutex::Owner lock(m_sConnStatsMgrLock);
		m_lstStats.PushBack(pStats);
	}
	return pStats;
}

void RTMPStatsMgr::Release(RTMPStats **ppConnStats)
{
	RTMPStats *pStats;

	ASSERT(ppConnStats && *ppConnStats);

	BCSpinMutex::Owner lock(m_sConnStatsMgrLock);
	pStats = *ppConnStats;
	pStats->RemoveFromList();
	delete pStats;
	*ppConnStats = NULL;
}

uint32_t RTMPStatsMgr::GetCount()
{
	BCSpinMutex::Owner lock(m_sConnStatsMgrLock);
	return m_lstStats.Count();
}

BCRESULT RTMPStatsMgr::EnumConnStats(LPFN_NetConnEnumPtr lpfnEnumFunc, void *lParam)
{
	RTMPStats *pStats, *pIterEnd;
	uint32_t nIndex = 0;
	BOOL bContinue = TRUE;

	BCSpinMutex::Owner lock(m_sConnStatsMgrLock);

	pStats = m_lstStats.Begin();
	pIterEnd = m_lstStats.End();
	for (; pStats != pIterEnd; pStats = m_lstStats.Next(pStats))
	{
		pStats->m_sStatsLock.Lock(bc_rwlocktype_read);
		pStats->m_nStatsSize = m_lstStats.Count();
		pStats->m_nStatsIndex = nIndex;
		bContinue = (lpfnEnumFunc)(*pStats, lParam);	
		pStats->m_sStatsLock.Unlock(bc_rwlocktype_read);
		if (!bContinue)
		{
			break;
		}	
		nIndex++;
	}
	return BC_R_SUCCESS;
}

///////////////////////////////////////////////////////////////////////////////
// End of namespace : RTMP
///////////////////////////////////////////////////////////////////////////////

} // End of namespace RTMP

///////////////////////////////////////////////////////////////////////////////
// End of file : RTMPStatsMgr.cpp
///////////////////////////////////////////////////////////////////////////////
