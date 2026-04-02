///////////////////////////////////////////////////////////////////////////////
// file : EventQueue.h
// author : antoniozhou
///////////////////////////////////////////////////////////////////////////////

#ifndef EVENTQUEUE_H_INCLUDED__
#define EVENTQUEUE_H_INCLUDED__

#include <memory>
#include <BC/BCList.h>
#include <BC/BCThread.h>
#include <BC/BCMemPool.h>
#include <BC/BCTask.h>

///////////////////////////////////////////////////////////////////////////////
// Namespace : BC
///////////////////////////////////////////////////////////////////////////////

namespace BC
{

///////////////////////////////////////////////////////////////////////////////
// Macros & typedefs
///////////////////////////////////////////////////////////////////////////////

#define MAKEEVENT(major, minor, sub)	MAKELONG(MAKEWORD(sub, minor), major)
#define EVENTMAJOR(lEvent)				HIWORD(lEvent)
#define EVENTMINOR(lEvent)				HIBYTE(LOWORD(lEvent))
#define EVENTSUB(lEvent)				LOBYTE(LOWORD(lEvent))

#define BC_EVENTCLASS_EVQUEUE			BC_EVENTCLASS(9)

#define BCM_IDLE	0

struct BCEventItemS;

typedef void (*BCEventProcPtr)(BCEventItemS &refEvent);
typedef BCEventProcPtr		LPFN_BCEventProcPtr;


typedef void(*EventNotifyProc)(void *data);
typedef EventNotifyProc		LPFN_EventNotifyProc;

typedef void (*BCEventCopyPtr)(const BCEventItemS &refEventFrom, BCEventItemS &refEventTo);
typedef BCEventCopyPtr		LPFN_BCEventCopyPtr;

///////////////////////////////////////////////////////////////////////////////
// struct : BCEventItemS
///////////////////////////////////////////////////////////////////////////////

struct BCEventItemS
{
	uint32_t				eType;
	uint64_t				wParam;
	uint64_t				lParam;
	uint64_t				vParams[8];
	uint64_t				priv;
	LPFN_BCEventProcPtr		cbDestroy;
	LPFN_BCEventCopyPtr		cbCopy;
	BCEventItemS();
	BCEventItemS(const BCEventItemS &other);
	BCEventItemS(
		uint32_t _eType,
		void *_wParam = NULL,
		void *_lParam = NULL,
		LPFN_BCEventProcPtr _cbDestroy = NULL,
		LPFN_BCEventCopyPtr _cbCopy = NULL);
	BCEventItemS(
		uint32_t _eType,
		uint64_t _wParam,
		uint64_t _lParam = 0,
		LPFN_BCEventProcPtr _cbDestroy = NULL,
		LPFN_BCEventCopyPtr _cbCopy = NULL);
	~BCEventItemS();

	BCEventItemS &operator=(const BCEventItemS &other);

	LPVOID			AllocBuffer(size_t nSize);
	LPSTR			CopyString(LPCSTR lpszStr, int len = -1);
	LPVOID			CopyBuffer(LPCVOID lpData, size_t len);
private:
	std::shared_ptr<KBPool> pool;
};

///////////////////////////////////////////////////////////////////////////////
// class : BCEventDispatcher
///////////////////////////////////////////////////////////////////////////////

class BC_API BCEventDispatcher
{
public:
	virtual	bool		PostEvent(
							uint32_t eCtrlType,
							uint64_t dwParam1,
							uint64_t dwParam2 = 0,
							LPFN_BCEventProcPtr cbDestroy = NULL);
	virtual	bool		PostEvent(
							uint32_t eCtrlType,
							void *wParam = NULL,
							void *lParam = NULL,
							LPFN_BCEventProcPtr cbDestroy = NULL);
	virtual	bool		PostEvent(const BCEventItemS &sEvent);
protected:
	virtual	bool		QueueEvent(const BCEventItemS &sEvent)		= 0;
	virtual bool		OnEventProcess(BCEventItemS &refEvent)		= 0;
};

///////////////////////////////////////////////////////////////////////////////
// class : BCEventFactory
///////////////////////////////////////////////////////////////////////////////

class BC_API BCEventFactory : public BCEventDispatcher
{
	enum
	{
		EVENT_STATE_IDLE		= 0,
		EVENT_STATE_RUNNING		= 1
	};
public:
	BCRESULT			Create(BCTaskMgr *pTaskMgr, const char *name, void *tag);
	BCTask			*	GetTask() const;
	void				Detach(bool bRemoveOnShutdown = false);
	void				FlushEvents(uint32_t eType = 0);
protected:
	BCEventFactory();
	virtual ~BCEventFactory();

	bool				QueueEvent(const BCEventItemS &refEvent);
	virtual bool		OnEventProcess(BCEventItemS &refEvent);
	virtual void		OnEventProcShutdown();
	void				_FlushEvents();

	uint32_t			m_nCtrls;
private:
	DECLARE_NO_COPY_CLASS(BCEventFactory);

	bool				_PopEvent(BCEventItemS &);
	static void			_EventCallback(BCTask *, BCTaskEvent *);
	static void			_ShutdownCallback(BCTask *, BCTaskEvent *);

	typedef BCList<BCEventItemS>	EventList;
	BCSpinMutex			m_sEventLock;
	EventList			m_lstEvents;
	BCTaskEvent			m_sTaskEvent;
	BCTask			*	m_pTask;
	uint32_t			m_nEventState;
};

BC_API bool BCDefEventProc(BCEventItemS &refEvent);


///////////////////////////////////////////////////////////////////////////////
// End of namespace : BC
///////////////////////////////////////////////////////////////////////////////

} // End of namespace BC


#endif // EVENTQUEUE_H_INCLUDED__

///////////////////////////////////////////////////////////////////////////////
// End of file : EventQueue.h
///////////////////////////////////////////////////////////////////////////////
