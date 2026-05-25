///////////////////////////////////////////////////////////////////////////////
// file : UVExchanger.h
// author : anto.
///////////////////////////////////////////////////////////////////////////////
#ifndef UVEXCHANGER_H_INCLUDED__
#define UVEXCHANGER_H_INCLUDED__

#include <BC/Exchanger.h>
#include <uv.h>


using namespace BC;

///////////////////////////////////////////////////////////////////////////////
// Namespace : node
///////////////////////////////////////////////////////////////////////////////

namespace node
{

///////////////////////////////////////////////////////////////////////////////
// class : UVEventFactory
///////////////////////////////////////////////////////////////////////////////

class UVEventFactory : public BCEventDispatcher
{
	friend class UVExchanger;
	enum 
	{ 
		EVENT_STATE_IDLE		= 0,
		EVENT_STATE_RUNNING		= 1
	};
public:
	BCRESULT			Create(uv_loop_t *loop);
	uv_loop_t		*	GetLoop() const;
	void				Detach();
	uint32_t			FlushEvents(IExchangeHandler *pHandler = NULL);
protected:
	UVEventFactory();
	virtual ~UVEventFactory();

	virtual bool		QueueEvent(const BCEventItemS &refEvent);
	virtual bool		OnEventProcess(BCEventItemS &refEvent);
	virtual void		OnUVClose(){};

	uint32_t			m_nCtrls;

	bool				_PopEvent(BCEventItemS &);
	uint32_t			_FlushEvents(IExchangeHandler *pHandler = NULL);
	static void			_EventCallback(uv_async_t* handle);
	static void			_Close(uv_handle_t *handle);

	typedef BCList<BCEventItemS>	EventList;
	BCSpinMutex			m_sEventLock;
	uv_loop_t		*	m_pLoop;
	EventList			m_lstEvents;
	uv_async_t			m_sTaskEvent;
	bool				m_bAvailable;
	uint32_t			m_nEventState;
private:
	DECLARE_NO_COPY_CLASS(UVEventFactory);
};

///////////////////////////////////////////////////////////////////////////////
// Class : UVExchanger
///////////////////////////////////////////////////////////////////////////////

class UVExchanger : public UVEventFactory
{
public:
	UVExchanger();
	~UVExchanger();

	static BCRESULT		CreateInstance(uv_loop_t *loop);
	static BCRESULT		ExchangeEvent(
							BCEventItemS &refEvent,
							IExchangeHandler *pHandler);
	static BCRESULT		ExchangeShutdown(IExchangeHandler *pHandler);
	static uint32_t		RemoveEventByHandler(
							IExchangeHandler *pHandler);
	static uv_loop_t *	GetLoop();
	static void			Destroy();
protected:
	// Overrided event handlers
	bool				OnEventProcess(BCEventItemS &refEvent);
	void				OnUVClose();
	static void			_ThreadProc(void *arg);
	BCRESULT			_Initialize();
private:
	DECLARE_NO_COPY_CLASS(UVExchanger);
	static BCSpinMutex		m_sLock;
	static UVExchanger	*	m_pInstance;
private:
	BCThread			*	m_pThread;
	BCSemaphore				m_startSem;
	BCMutex					m_exitLock;
	BCCondition				m_exitCond;
};

///////////////////////////////////////////////////////////////////////////////
// End of namespace : node
///////////////////////////////////////////////////////////////////////////////

} // End of namespace : node

#endif // UVEXCHANGER_H_INCLUDED__

///////////////////////////////////////////////////////////////////////////////
// End of file : UVExchanger.h
///////////////////////////////////////////////////////////////////////////////
