
#ifndef BCTASK_INCLUDE__
#define BCTASK_INCLUDE__

#include "BC/Exports.h"
#include "BC/BCMemPool.h"
#include "BC/BCThread.h"
#include "BC/BCNodeList.h"
#include "BC/BCMagic.h"
#include "BC/BCTaskEvent.h"

///////////////////////////////////////////////////////////////////////////////
// Namespace : BC
///////////////////////////////////////////////////////////////////////////////

namespace BC
{

///////////////////////////////////////////////////////////////////////////////
// Macros :
///////////////////////////////////////////////////////////////////////////////

#define BC_TASKEVENT_FIRSTEVENT		(BC_EVENTCLASS_TASK + 0)
#define BC_TASKEVENT_SHUTDOWN		(BC_EVENTCLASS_TASK + 1)
#define BC_TASKEVENT_LASTEVENT		(BC_EVENTCLASS_TASK + 65535)

#define BC_TASK_MAGIC			BC_MAGIC('t', 'a', 's', 'k')
#define BC_TASKMGR_MAGIC		BC_MAGIC('t', 's', 'm', 'g')

class BCTask;
class BCTaskMgr;
class BCTaskEvent;

typedef enum BCTaskStateE
{
	task_state_idle			= 0,
	task_state_ready,
	task_state_running,
	task_state_done
} BCTaskStateE;

///////////////////////////////////////////////////////////////////////////////
// class : BCTask
///////////////////////////////////////////////////////////////////////////////

class BC_API BCTask
	: public BCNodeList::Node
	, public BCMagic

{
	friend class BCTaskMgr;
public:
	BCTask();
	virtual ~BCTask();

	BCRESULT		Create(BCTaskMgr *pMgr, uint32_t nQuantum);
	void			Attach(BCTask **ppTarget);
	void			Detach(BCTask **taskp);
	void			Send(BCTaskEvent **eventp);
	void			SendAndDetach(BCTaskEvent **eventp);
	uint32_t		Unsend(
						void *sender,
						BCEventType type,
						void *tag,
						BCTaskEventList &refLstEvents);
	uint32_t		PurgeRange(
						void *pSender,
						BCEventType first,
						BCEventType last,
						void *pTag);
	uint32_t		Purge(
						void *pSender,
						BCEventType type,
						void *pTag);
	BOOL			PurgeEvent(BCTaskEvent *pEvent);
	uint32_t		UnsendRange(
						void *sender,
						BCEventType first,
						BCEventType last,
						void *tag,
						BCTaskEventList &refLstEvents);
	BCRESULT		OnShutdown(
						LPFN_BCTaskAction action,
						const void *arg);
	void			RemoveAllOnShutdown();
	void			Shutdown();
	void			Destroy(BCTask **taskp);
	void			SetName(const char *name, void *tag);
	const char	*	GetName() const;
	void		*	GetTag() const;
	void			GetCurTime(uint32_t *pTime);
	BCRESULT		BeginExclusive();
	void			EndExclusive();
	BOOL			IsExiting() const;
/*%<
 * Returns BC_TRUE if the task is in the process of shutting down,
 * BC_FALSE otherwise.
 *
 * Requires:
 *\li	'task' is a valid task.
 */
	BCTaskMgr	*	GetManager() const;

protected:
	inline BOOL		_Shutdown();
	inline void		_Ready();
	inline BOOL		_Detach();
	inline BOOL		_Send(BCTaskEvent **ppEvent);
	uint32_t		_DequeueEvents(
						void *pSender,
						BCEventType first,
						BCEventType last,
						void *pTag,
						BCTaskEventList &refLstEvents,
						BOOL bPurging);
private:
	DECLARE_NO_COPY_CLASS(BCTask);
	/* Not locked. */
	BCTaskMgr		*	m_pMgr;
	BCSpinMutex			m_sLock;
	/* Locked by task m_sLock. */
	BCTaskStateE		m_eState;
	uint32_t			m_nRef;
	BCTaskEventList		m_lstEvents;
	BCTaskEventList		m_lstOnShutdown;
	uint32_t			m_nQuantum;
	uint32_t			m_nFlags;
	uint32_t			m_nNowTime;
	char				m_szName[16];
	void *				m_pTag;
};

typedef TNodeList<BCTask>		BCTaskList;

///////////////////////////////////////////////////////////////////////////////
// class : BCTaskMgr
///////////////////////////////////////////////////////////////////////////////

class BC_API BCTaskMgr : public BCMagic
{
	friend class BCTask;
public:
	BCTaskMgr();
	virtual ~BCTaskMgr();

	BCRESULT		Create(
						uint32_t nWorkers,
						uint32_t nDefQuantam,
						BCThread::priority_t ePri = BCThread::PRIORITY_NORMAL,
						LPCSTR lpszThreadName = "");
	static void		Destroy(BCTaskMgr **managerp);
	uint32_t		GetTaskCount() const;

protected:
	void			_Free();
	void			_Dispatch();
	void			_Finished(BCTask *pTask);
	static void	*	_Run(void *pArg);
private:
	DECLARE_NO_COPY_CLASS(BCTaskMgr);
	/* Not locked. */
	BCMutex					m_sLock;
	uint32_t				m_nWorkers;
	KBPool					m_sPool;
	BCThread			**	m_ppThreads;
	/* Locked by task m_pManager m_sLock. */
	uint32_t				m_nDefQuantum;
	BCTaskList				m_lstTasks;
	BCTaskList				m_lstReadyTasks;
	BCCondition				m_sCondWorkAvailable;
	BCCondition				m_sCondExclusiveGranted;
	uint32_t				m_nRunningTasks;
	BOOL					m_bExclusiveRequested;
	BOOL					m_bExiting;
};

///////////////////////////////////////////////////////////////////////////////
// End of namespace :
///////////////////////////////////////////////////////////////////////////////

};

#endif

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////