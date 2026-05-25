
#ifndef BC_NODELIST_INCLUDE__
#define BC_NODELIST_INCLUDE__


#include "BC/Exports.h"

///////////////////////////////////////////////////////////////////////////////
// Namespace: BC
///////////////////////////////////////////////////////////////////////////////

namespace BC
{

///////////////////////////////////////////////////////////////////////////////
// CNodeList
///////////////////////////////////////////////////////////////////////////////

class BC_API BCNodeList
{
public :
	///////////////////////////////////////////////////////////////////////////////
	// class : BCNodeList::Node
	///////////////////////////////////////////////////////////////////////////////
	class BC_API Node
	{
	public:
		Node	*	Prev() const;
		Node	*	Next() const;
		void		Next(Node *pNext);
		void		AddToList(BCNodeList *pList);
		void		RemoveFromList();
		bool		IsLinked() const;
		Node	*	ReplaceBy(Node *pNode);

	protected:
		Node();
		virtual ~Node();

	private :
		friend class BCNodeList;

		void		Unlink();
		void		SetNext(Node *pNext);
		void		SetPrev(Node *pPrev);

		Node			*	m_pNext;
		Node			*	m_pPrev;
		BCNodeList		*	m_pList;
	};

	BCNodeList();
	virtual ~BCNodeList();

	Node		*	operator [] (uint32_t nIndex) const;
	Node		*	Insert( Node *pCurrent, Node *pNode );
	void			PushFront(Node *pNode);
	void			PushBack(Node *pNode);
	Node		*	PopFront();
	Node		*	PopBack();
	Node		*	Front() const;
	Node		*	Back() const;
	Node		*	Begin() const;
	Node		*	End() const;
	Node		*	Replace(Node *pOrig, Node *pReplaceBy);
	size_t			Count() const;
	bool			IsEmpty() const;
	bool			IsExist(const Node *pNode) const;
	void			Clear();

private :
	friend void Node::RemoveFromList();
	void RemoveNode(Node *pNode);
	// for test
	inline void CheckValid();

	Node						m_Head;
	mutable Node				m_Tail;
	size_t						m_numNodes;
};

///////////////////////////////////////////////////////////////////////////////
// TNodeList
///////////////////////////////////////////////////////////////////////////////

template <class T>
class TNodeList : public BCNodeList
{
public :
	TNodeList(){};
	~TNodeList(){};

	T		*	operator [] (uint32_t nIndex) const;
	T		*	PopFront();
	T		*	PopBack();
	T		*	Front() const;
	T		*	Back() const;
	T		*	Begin() const;
	T		*	End() const;
	T		*	Replace(T *pOrig, T *pReplaceBy);
	bool		IsExist(const T *pNode) const;
	static T *	Prev(const T *pNode);
	static T *	Next(const T *pNode);
private:
	// NO COPY CLASS DECLARE
	TNodeList(const TNodeList<T>&);
	//operator =(const TNodeList<T>&);
};

template <class T>
T *TNodeList<T>::operator [] (uint32_t nIndex) const
{
	return static_cast<T *>(BCNodeList::operator[](nIndex));
}

template <class T>
T *TNodeList<T>::PopFront()
{
	return static_cast<T *>(BCNodeList::PopFront());
}

template <class T>
T *TNodeList<T>::PopBack()
{
	return static_cast<T *>(BCNodeList::PopBack());
}

template <class T>
T *TNodeList<T>::Front() const
{
	return static_cast<T *>(BCNodeList::Front());
}

template <class T>
T *TNodeList<T>::Back() const
{
	return static_cast<T *>(BCNodeList::Back());
}

template <class T>
T *TNodeList<T>::Begin() const
{
	return static_cast<T *>(BCNodeList::Begin());
}

template <class T>
T *TNodeList<T>::End() const
{
	return static_cast<T *>(BCNodeList::End());
}

template <class T>
bool TNodeList<T>::IsExist(const T *pNode) const
{
	return BCNodeList::IsExist(static_cast<const Node*>(pNode));
}

template <class T>
T *TNodeList<T>::Prev(const T *pNode)
{
	return static_cast<T*>(pNode->Prev());
}

template <class T>
T *TNodeList<T>::Next(const T *pNode)
{
	return static_cast<T*>(pNode->Next());
}

template <class T>
T *TNodeList<T>::Replace(T *pOrig, T *pReplaceBy)
{
	return static_cast<T*>(BCNodeList::Replace(pOrig, pReplaceBy));
}

///////////////////////////////////////////////////////////////////////////////
// Namespace: BC
///////////////////////////////////////////////////////////////////////////////

} // End of namespace BC


#endif //BC_NODELIST_INCLUDE__

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
