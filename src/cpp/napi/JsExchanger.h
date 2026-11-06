


#ifndef EVENTEXCHANGER_H_INCLUDED__
#define EVENTEXCHANGER_H_INCLUDED__

#include "UVExchanger.h"


using namespace BC;





namespace node
{





class JsExchanger : public UVEventFactory
{
public:
	JsExchanger();
	~JsExchanger();

	static BCRESULT		CreateInstance();
	static BCRESULT		ExchangeEvent(
							BCEventItemS &refEvent,
							IExchangeHandler *pHandler);
	static BCRESULT		ExchangeShutdown(IExchangeHandler *pHandler);
	static uint32_t		RemoveEventByHandler(IExchangeHandler *pHandler);
	uint32_t			_RemoveEventByHandler(IExchangeHandler *pHandler);
protected:
	
	bool				OnEventProcess(BCEventItemS &refEvent);
private:
	DECLARE_NO_COPY_CLASS(JsExchanger);
	static BCSpinMutex		m_sLock;
	static JsExchanger	*	m_pInstance;
};





} 

#endif 




