
///////////////////////////////////////////////////////////////////////////////
// File : BCRuntime.h
///////////////////////////////////////////////////////////////////////////////

#ifndef BC_BCRUNTIME_H_INCLUDED__
#define BC_BCRUNTIME_H_INCLUDED__

#include <BC/Exports.h>

///////////////////////////////////////////////////////////////////////////////
// Namespace : BC
///////////////////////////////////////////////////////////////////////////////

namespace BC
{

class BCTaskMgr;
class BCTimerMgr;
class BCSocketMgr;

#define BCRTC_UGTSKMGR		0x00000001
#define BCRTC_TSKMGR		0x00000002
#define BCRTC_TIMERMGR		0x00000004
#define BCRTC_SOCKMGR		0x00000008
#define BCRTC_RATELIMITER	0x00000010
#define BCRTC_NOUGTIMER		BCRTC_TSKMGR|BCRTC_TIMERMGR
#define BCRTC_NONETWORK		BCRTC_UGTSKMGR|BCRTC_NOUGTIMER
#define BCRTC_ALL			BCRTC_NONETWORK|BCRTC_SOCKMGR|BCRTC_RATELIMITER

///////////////////////////////////////////////////////////////////////////////
// Class : BCRuntime
///////////////////////////////////////////////////////////////////////////////

class BC_API BCRuntime 
{
public:
	BCRuntime();
	virtual ~BCRuntime();

	virtual BCRESULT	Create(
							uint32_t nFlags, 
							uint32_t nUgWorkers, 
							uint32_t nWorkers);
	virtual BCRESULT	Create(
							BCTaskMgr *pTaskMgr, 
							BCTaskMgr *pUgTaskMgr,
							BCTimerMgr *pTimerMgr, 
							BCSocketMgr *pSockMgr);
	inline uint32_t		GetFlags(){ return m_nRTCFlags; }
	void				Destroy();

	BCTaskMgr			*	m_pUgTaskMgr;
	BCTaskMgr			*	m_pTaskMgr;
	BCTimerMgr			*	m_pTimerMgr;
	BCSocketMgr			*	m_pSockMgr;
private:
	DECLARE_NO_COPY_CLASS(BCRuntime);
	uint32_t				m_nRTCFlags;
};

///////////////////////////////////////////////////////////////////////////////
// End of namespace : BC
///////////////////////////////////////////////////////////////////////////////

} // End of namespace : BC

#endif // BC_BCRUNTIME_H_INCLUDED__

///////////////////////////////////////////////////////////////////////////////
// End of file : BCRuntime.h
///////////////////////////////////////////////////////////////////////////////
