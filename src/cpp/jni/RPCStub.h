



#ifndef RPCSTUB_H_INCLUDED__
#define RPCSTUB_H_INCLUDED__

#include <BC/BCMap.h>
#include "Interface.h"






namespace JNI
{






class RPCStub : public IRPCStub
{
public:
	RPCStub(uint32_t nTransId);
	~RPCStub();

	BCRESULT		Create(LPCSTR lpCmd);

private:
	DECLARE_NO_COPY_CLASS(RPCStub);
};





class RPCStubMgr
{
public:
	RPCStubMgr();
	~RPCStubMgr();

	RPCStub		*	GetStub(uint32_t nTransId = 0);
	void			PutStub(uint32_t nTransId, BOOL bReuseId = FALSE);
	RPCStub		*	RemoveStub(uint32_t nTransId);
	void			Clear();

protected:
private:
	DECLARE_NO_COPY_CLASS(RPCStubMgr);
	typedef BCMap<uint32_t, RPCStub *>	RPCStubMap;
	RPCStubMap			m_mapStubs;
	uint32_t			m_nNextProbId;
};






} 

#endif 




