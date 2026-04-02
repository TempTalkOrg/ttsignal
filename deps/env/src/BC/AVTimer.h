///////////////////////////////////////////////////////////////////////////////
// file : AVTimer.h
// author : antoniozhou
///////////////////////////////////////////////////////////////////////////////

#ifndef BC_AVTIMER_H_INCLUDED__
#define BC_AVTIMER_H_INCLUDED__

#include <BC/Utils.h>
#include <BC/BCThread.h>
#include <BC/BCNodeList.h>

///////////////////////////////////////////////////////////////////////////////
// Namespace : BC
///////////////////////////////////////////////////////////////////////////////

namespace BC 
{

class AVTimer;

///////////////////////////////////////////////////////////////////////////////
// class : IAVTimerHandler
///////////////////////////////////////////////////////////////////////////////

class IAVTimerHandler
{
public:
	IAVTimerHandler(){};
	virtual ~IAVTimerHandler(){};

	virtual bool		OnAVTimer(
							uint32_t id, 
							int64_t time,
							uint64_t userData)		= 0;

private:
	DECLARE_NO_COPY_CLASS(IAVTimerHandler);
};

///////////////////////////////////////////////////////////////////////////////
// class : AVTimerStub
///////////////////////////////////////////////////////////////////////////////

class AVTimerStub : public BCNodeList::Node
{
public:
	AVTimerStub(
		uint32_t intervalUS, 
		IAVTimerHandler *pHandler, 
		uint32_t id_,
		uint64_t userData_) 
		: due(0), ticks(0), interval(intervalUS), handler(pHandler)
		, id(id_), userData(userData_)
	{
		due = bc_time_now();
	}
	~AVTimerStub() {
		//
	}

	void		Start() {

	}
	bool		Trigger(int64_t time) {
		bool bContinue = false;
		if (handler)
		{
			bContinue = handler->OnAVTimer(id, time, userData);
			ticks++;
		}
		return bContinue;
	}
	bool		IsBefore(int64_t time) {
		return due < time;
	}

	int64_t					due;
	uint64_t				ticks;
	int32_t					interval;
	IAVTimerHandler		*	handler;
	uint32_t				id;
	uint64_t				userData;
};

class AVTimerStubQueue
{
	friend class AVTimerStub;
public:
	AVTimerStubQueue();
	~AVTimerStubQueue();

	// CheckTimeouts()
	//   Dispatches any elapsed Timers, and returns the number of milliseconds until the
	//   next AVTimerStub will timeout.
	int				CheckTimeouts();

	void			ClearTimers();

	BCRESULT		AddTimer(
						IAVTimerHandler *pHandler, 
						int32_t interval,
						uint32_t &id,
						uint64_t userData);
	BCRESULT		RemoveTimer(
						IAVTimerHandler *pHandler,
						uint32_t id);
protected:

	void			Stop(AVTimerStub *pTimer);

	bool			IsStarted(AVTimerStub *pTimer);

	void			_InsertTimer(AVTimerStub* t);

	// GetNextTimeout()
	//   Returns the number of milliseconds until the next timeout, without dispatching
	//   any elapsed Timers.
	int				_GetNextTimeout();
private:
	BCSpinMutex					m_sLock;
	// The list of currently active Timers, ordered by time left until timeout.
	typedef TNodeList<AVTimerStub>	TimerList;
	TimerList					m_lstTimers;
	uint32_t					m_nNextTimerId;
};

///////////////////////////////////////////////////////////////////////////////
// class : AVTimer
///////////////////////////////////////////////////////////////////////////////

class BC_API AVTimer
{
public:
	AVTimer();
	~AVTimer();

	static BCRESULT		Start();
	static BCRESULT		Stop();
	static BCRESULT		AddListener(
							IAVTimerHandler *pHandler, 
							uint32_t interval,
							uint32_t &id,
							uint64_t userData = 0);
	static BCRESULT		RemoveListener(
							IAVTimerHandler *pHandler,
							uint32_t id);

protected:
	BCRESULT			_AddListener(
							IAVTimerHandler *pHandler, 
							uint32_t interval,
							uint32_t &id,
							uint64_t userData);
	BCRESULT			_RemoveListener(
							IAVTimerHandler *pHandler,
							uint32_t id);
	void				_RunTimer();
private:
	static void			_InitOnce(void*);
	static void			_ThreadProc(LPVOID lpArg);
private:
	DECLARE_NO_COPY_CLASS(AVTimer);
	BCMutex					m_sCondLock;
	BCCondition				m_sCond;
	AVTimerStubQueue		m_queueTimers;
	BCTimeS					m_tmDue;
	BCCondition				m_sStartCond;
	BCCondition				m_sExitCond;
	bool					m_bExit;
	// static members
	static BCOnceS			m_sInitOnce;
	static BCThread		*	m_pThread;
	static AVTimer		*	m_pInstance;
};


///////////////////////////////////////////////////////////////////////////////
// End of namespace : BC
///////////////////////////////////////////////////////////////////////////////

} // End of namespace BC

#endif // BC_AVTIMER_H_INCLUDED__

///////////////////////////////////////////////////////////////////////////////
// End of file : AVTimer.h
///////////////////////////////////////////////////////////////////////////////
