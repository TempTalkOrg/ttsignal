

#include "BC/BCNodeList.h"


///////////////////////////////////////////////////////////////////////////////
// Namespace: BC
///////////////////////////////////////////////////////////////////////////////

namespace BC
{

//#define DO_CHECK_VALID
// for test
#ifdef DO_CHECK_VALID
#define CHECKVALID(x)			x
#else
#define CHECKVALID(x)
#endif

///////////////////////////////////////////////////////////////////////////////
// CNodeList
///////////////////////////////////////////////////////////////////////////////

BCNodeList::BCNodeList()
		: m_numNodes(0)
{
	m_Head.m_pNext = &m_Tail;
	m_Tail.m_pPrev = &m_Head;
}

BCNodeList::~BCNodeList()
{
	//
}

BCNodeList::Node *BCNodeList::operator [](uint32_t nIndex) const
{
	uint32_t nIter = 0;
	Node *pIter = Begin();
	Node *pEnd = End();
	for (; nIter < nIndex && pIter != pEnd; nIter++)
	{
		pIter = pIter->Next();
	}
	if (nIter == nIndex && pIter != pEnd)
	{
		return pIter;
	}
	return NULL;
}

// Insert x before pCurrent.
BCNodeList::Node *BCNodeList::Insert( Node *pCurrent, Node *pNode )
{
	if (!pNode)
	{
		return NULL;
	}
	pNode->RemoveFromList();
	pNode->AddToList(this);
	pNode->m_pPrev = pCurrent->m_pPrev;
	pNode->m_pNext = pCurrent;
	pCurrent->m_pPrev->m_pNext = pNode;
	pCurrent->m_pPrev = pNode;
	m_numNodes++;
	ASSERT(pNode);
	ASSERT(pNode->m_pPrev);
	ASSERT(pNode->m_pNext);
	CHECKVALID(CheckValid());
	return pNode;
}

void BCNodeList::PushFront(
    Node *pNode)
{
	if (pNode)
	{
		Insert(m_Head.m_pNext, pNode);
	}
}

void BCNodeList::PushBack(
	Node *pNode)
{
	if (pNode)
	{
		Insert(&m_Tail, pNode);
	}
}

BCNodeList::Node *BCNodeList::PopFront()
{
	Node *pNode = m_Head.Next();

	if (pNode && pNode != &m_Tail)
	{
		ASSERT(pNode->m_pNext);
		RemoveNode(pNode);
		return pNode;
	}

	return NULL;
}

BCNodeList::Node *BCNodeList::PopBack()
{
	Node *pNode = m_Tail.m_pPrev;

	if (pNode && pNode != &m_Head)
	{
		ASSERT(m_Tail.m_pPrev);
		RemoveNode(pNode);
		return pNode;
	}

	return NULL;
}

BCNodeList::Node *BCNodeList::Front() const
{
	Node *pNode = m_Head.Next();

	if (pNode && pNode != &m_Tail)
	{
		return pNode;
	}

	return NULL;
}

BCNodeList::Node *BCNodeList::Back() const
{
	Node *pNode = m_Tail.m_pPrev;

	if (pNode && pNode != &m_Head)
	{
		return pNode;
	}

	return NULL;
}

BCNodeList::Node *BCNodeList::Begin() const
{
	return m_Head.m_pNext;
}

BCNodeList::Node *BCNodeList::End() const
{
	return &m_Tail;
}

BCNodeList::Node *BCNodeList::Replace(Node *pOrig, Node *pReplaceBy)
{
	ASSERT(pOrig && pOrig->m_pList == this);
	ASSERT(pReplaceBy);
	return pOrig->ReplaceBy(pReplaceBy);
}

size_t BCNodeList::Count() const
{
	return m_numNodes;
}

bool BCNodeList::IsEmpty() const
{
	return (0 == m_numNodes);
}

bool BCNodeList::IsExist(const Node *pNode) const
{
	if (NULL == pNode)
	{
		return false;
	}

	Node *pNext = Begin();
	Node *pEnd = End();
	while(pNext && pNext != pEnd)
	{
		if (pNext == pNode)
		{
			return true;
		}
		pNext = pNext->Next();
	}
	return false;
}

void BCNodeList::RemoveNode(Node *pNode)
{
	if (NULL == pNode)
	{
		return;
	}
	pNode->Unlink();
	m_numNodes--;
	CHECKVALID(CheckValid());
}

void BCNodeList::Clear()
{
	while(PopFront()){}
}

inline void BCNodeList::CheckValid()
{
	ASSERT(m_Head.m_pNext);
	ASSERT(m_Tail.m_pPrev);
}

///////////////////////////////////////////////////////////////////////////////
// CNodeList::Node
///////////////////////////////////////////////////////////////////////////////

BCNodeList::Node::Node()
		: m_pNext(0)
		, m_pPrev(0)
		, m_pList(0)
{
}

BCNodeList::Node::~Node()
{
	try
	{
		RemoveFromList();
	}
	catch (...)
	{
	}

	m_pNext = 0;
	m_pPrev = 0;
	m_pList = 0;
}

BCNodeList::Node *BCNodeList::Node::Prev() const
{
	return m_pPrev;
}

BCNodeList::Node *BCNodeList::Node::Next() const
{
	return m_pNext;
}

void BCNodeList::Node::Next(Node *pNext)
{
	m_pNext = pNext;

	if (pNext)
	{
		pNext->SetPrev(this);
	}
}

void BCNodeList::Node::AddToList(BCNodeList *pList)
{
	m_pList = pList;
}

void BCNodeList::Node::RemoveFromList()
{
	if (m_pList)
	{
		m_pList->RemoveNode(this);
	}
}

void BCNodeList::Node::Unlink()
{
	if (m_pPrev)
	{
		m_pPrev->m_pNext = m_pNext;
	}

	if (m_pNext)
	{
		m_pNext->m_pPrev = m_pPrev;
	}

	m_pNext = 0;
	m_pPrev = 0;
	m_pList = 0;
}

void BCNodeList::Node::SetNext(Node *pNext)
{
	m_pNext = pNext;
}

void BCNodeList::Node::SetPrev(Node *pPrev)
{
	m_pPrev = pPrev;
}

bool BCNodeList::Node::IsLinked() const
{
	return (m_pList != NULL);
}

BCNodeList::Node *BCNodeList::Node::ReplaceBy(Node *pNode)
{
	ASSERT(pNode);

	pNode->Unlink();
	if (m_pPrev)
	{
		m_pPrev->m_pNext = pNode;
		pNode->m_pPrev = m_pPrev;
	}
	if (m_pNext)
	{
		m_pNext->m_pPrev = pNode;
		pNode->m_pNext = m_pNext;
	}
	pNode->m_pList = m_pList;

	m_pNext = 0;
	m_pPrev = 0;
	m_pList = 0;

	return this;
}

///////////////////////////////////////////////////////////////////////////////
// Namespace: BC
///////////////////////////////////////////////////////////////////////////////

} // End of namespace BC


///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
