///////////////////////////////////////////////////////////////////////////////
// file : RPCStub.cpp
// author : anto.
///////////////////////////////////////////////////////////////////////////////

#include "../StdAfx.h"
#include "RPCStub.h"


///////////////////////////////////////////////////////////////////////////////
// Namespace : node
///////////////////////////////////////////////////////////////////////////////

namespace node
{

///////////////////////////////////////////////////////////////////////////////
// class : RPCStub
///////////////////////////////////////////////////////////////////////////////

RPCStub::RPCStub(uint32_t nTransId)
	: IRPCStub(nTransId)
{
	//
}

RPCStub::~RPCStub()
{
	m_fCallback.Reset();
	if (m_lpfnDtor)
	{
		(m_lpfnDtor)(*this);
	}
}

BCRESULT RPCStub::Create(LPCSTR lpCmd)
{
	ASSERT(lpCmd);

	strcpy(m_szCmd, lpCmd);
	m_fCallback.Reset();

	return BC_R_SUCCESS;
}

BCRESULT RPCStub::Create(LPCSTR lpCmd, Napi::Function callback)
{
	ASSERT(lpCmd);
	ASSERT(!callback.IsEmpty());

	strcpy(m_szCmd, lpCmd);
	m_fCallback.Reset(callback, 1);

	return BC_R_SUCCESS;
}

///////////////////////////////////////////////////////////////////////////////
// class : RPCStubMgr
///////////////////////////////////////////////////////////////////////////////

RPCStubMgr::RPCStubMgr()
	: m_nNextProbId(1)
{
	//
}

RPCStubMgr::~RPCStubMgr()
{
	Clear();
}

RPCStub *RPCStubMgr::GetStub(uint32_t nId)
{
	uint32_t nTransId;
	RPCStub *pStub;

	if (nId == 0)
	{
		nTransId = m_nNextProbId++;
		pStub = new RPCStub(nTransId);
		ASSERT(pStub);
		m_mapStubs[nTransId] = pStub;
	}
	else
	{
		RPCStubMap::iterator iter, iterEnd;
		iterEnd = m_mapStubs.end();
		iter = m_mapStubs.find(nId);
		if (iter != iterEnd)
		{
			pStub = iter->second;
			ASSERT(pStub);
		}
		else
		{
			pStub = NULL;
		}
	}
	return pStub;
}

void RPCStubMgr::PutStub(uint32_t nTransId, BOOL bReuseId)
{
	RPCStubMap::iterator iter, iterEnd;

	ASSERT(nTransId > 0);
	iterEnd = m_mapStubs.end();
	iter = m_mapStubs.find(nTransId);
	ASSERT(iter != iterEnd);
	delete iter->second;
	m_mapStubs.erase(iter);
	if (m_mapStubs.size() == 0)
	{
		m_mapStubs.clear();
		if (bReuseId)
		{
			m_nNextProbId = 1;
		}
	}
}

RPCStub *RPCStubMgr::RemoveStub(uint32_t nTransId)
{
	RPCStubMap::iterator iter, iterEnd;
	RPCStub *pStub = NULL;

	iterEnd = m_mapStubs.end();
	iter = m_mapStubs.find(nTransId);
	if (iter != iterEnd)
	{
		pStub = iter->second;
		m_mapStubs.erase(iter);
		if (m_mapStubs.size() == 0)
		{
			m_mapStubs.clear();
		}
	}
	return pStub;
}

void RPCStubMgr::Clear()
{
	RPCStubMap::iterator iter, iterEnd;

	iter = m_mapStubs.begin();
	iterEnd = m_mapStubs.end();
	for (;iter != iterEnd;iter++)
	{
		delete iter->second;
	}
	m_mapStubs.clear();
	m_nNextProbId = 1;
}

///////////////////////////////////////////////////////////////////////////////
// End of namespace : node
///////////////////////////////////////////////////////////////////////////////

} // End of namespace node

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
