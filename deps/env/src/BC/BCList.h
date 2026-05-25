
#ifndef BC_BCLIST_H_INCLUDED__
#define BC_BCLIST_H_INCLUDED__


#include "BC/Exports.h"
#include "BC/Utils.h"

///////////////////////////////////////////////////////////////////////////////
// Namespace: BC
///////////////////////////////////////////////////////////////////////////////

namespace BC
{

///////////////////////////////////////////////////////////////////////////////
// SimpleList
///////////////////////////////////////////////////////////////////////////////

template <typename Object>
class BCList
{
private:
	// The basic doubly linked list node.
	// Nested inside of PoolList, can be public
	// because the Node is itself private
	struct Node
	{
		Object  data;
		Node   *prev;
		Node   *next;

		Node() : prev(NULL), next(NULL){}
		~Node(){}
	};

	class allocator
	{
		friend class BCList;
	public:
		allocator()
		{
			//
		}
		~allocator()
		{
			clear();
		}
		Node *alloc()
		{
			return new Node();
		}
		void dealloc(Node *pNode)
		{
			delete pNode;
		}
		void clear()
		{
			//
		}
	private:
	};

public:
	class const_iterator
	{
	public:

		// Public constructor for const_iterator.
		const_iterator( ) : current( NULL )
		{ }

		// Return the object stored at the current position.
		// For const_iterator, this is an accessor with a
		// const reference return type.
		const Object & operator* ( ) const
		{
			return retrieve( );
		}

		const_iterator & operator++ ( )
		{
			current = current->next;
			return *this;
		}

		const_iterator operator++ ( int )
		{
			const_iterator old = *this;
			++( *this );
			return old;
		}

		const_iterator & operator-- ( )
		{
			current = current->prev;
			return *this;
		}

		const_iterator operator-- ( int )
		{
			const_iterator old = *this;
			--( *this );
			return old;
		}

		bool operator== ( const const_iterator & rhs ) const
		{
			return current == rhs.current;
		}

		bool operator!= ( const const_iterator & rhs ) const
		{
			return !( *this == rhs );
		}

	protected:
		Node *current;

		// Protected helper in const_iterator that returns the object
		// stored at the current position. Can be called by all
		// three versions of operator* without any type conversions.
		Object & retrieve( ) const
		{
			return current->data;
		}

		// Protected constructor for const_iterator.
		// Expects a pointer that represents the current position.
		const_iterator( const Node *p ) :  current((Node *) p )
		{ }

		friend class BCList<Object>;
	};

	class iterator : public const_iterator
	{
	public:

		// Public constructor for iterator.
		// Calls the base-class constructor.
		// Must be provided because the private constructor
		// is written; otherwise zero-parameter constructor
		// would be disabled.
		iterator( )
		{ }

		Object & operator* ( )
		{
			return retrieve( );
		}

		// Return the object stored at the current position.
		// For iterator, there is an accessor with a
		// const reference return type and a mutator with
		// a reference return type. The accessor is shown first.
		const Object & operator* ( ) const
		{
			return const_iterator::operator*( );
		}

		iterator & operator++ ( )
		{
			current = current->next;
			return *this;
		}

		iterator operator++ ( int )
		{
			iterator old = *this;
			++( *this );
			return old;
		}

		iterator & operator-- ( )
		{
			current = current->prev;
			return *this;
		}

		iterator operator-- ( int )
		{
			iterator old = *this;
			--( *this );
			return old;
		}

	protected:
		// Protected constructor for iterator.
		// Expects the current position.
		iterator( Node *p ) : const_iterator( p )
		{ }

		friend class BCList<Object>;
		using	const_iterator::current;
		using	const_iterator::retrieve;
	};

public:
	BCList()
		: m_sAlloc()
	{
		init( );
	}

	BCList(
		const BCList & rhs)
		: m_sAlloc()
	{
		init( );
		*this = rhs;
	}

	~BCList( )
	{
		clear( );
	}

	const BCList & operator= ( const BCList & rhs )
	{
		if ( this == &rhs )
			return *this;
		clear( );
		const_iterator _rhs_Last = rhs.end( );
		for ( const_iterator itr = rhs.begin( ); itr != _rhs_Last; ++itr )
			push_back( *itr );
		return *this;
	}

	// Return iterator representing beginning of list.
	// Mutator version is first, then accessor version.
	iterator begin( )
	{
		return iterator( m_Head.next );
	}

	const_iterator begin( ) const
	{
		return const_iterator( m_Head.next );
	}

	// Return iterator representing endmarker of list.
	// Mutator version is first, then accessor version.
	iterator end( )
	{
		return iterator( &m_Tail );
	}

	const_iterator end( ) const
	{
		return const_iterator( &m_Tail );
	}

	// Return number of elements currently in the list.
	size_t size( ) const
	{
		return m_Size;
	}

	// Return true if the list is empty, false otherwise.
	bool empty( ) const
	{
		return size( ) == 0;
	}

	void clear( )
	{
		while ( !empty( ) )
			pop_front( );
	}

	// front, back, push_front, push_back, pop_front, and pop_back
	// are the basic double-ended queue operations.
	Object & front( )
	{
		return *begin( );
	}

	const Object & front( ) const
	{
		return *begin( );
	}

	Object & back( )
	{
		return *--end( );
	}

	const Object & back( ) const
	{
		return *--end( );
	}

	void push_front( const Object & x )
	{
		insert( begin( ), x );
	}

	void push_back( const Object & x )
	{
		insert( end( ), x );
	}

	void pop_front( )
	{
		erase( begin( ) );
	}

	void pop_back( )
	{
		erase( --end( ) );
	}

	// Insert x before itr.
	iterator insert( iterator itr, const Object & x )
	{
		Node *p = itr.current;
		Node *pNode = m_sAlloc.alloc();
		pNode->data = x;
		pNode->prev = p->prev;
		pNode->next = p;
		p->prev->next = pNode;
		p->prev = pNode;
		m_Size++;
		return iterator(pNode);
	}

	// Erase item at itr.
	iterator erase( iterator itr )
	{
		Node *p = itr.current;
		iterator retVal(p->next);
		p->prev->next = p->next;
		p->next->prev = p->prev;
		m_sAlloc.dealloc(p);
		m_Size--;
		if (0 == m_Size)
		{
			m_sAlloc.clear();
		}

		return retVal;
	}

	iterator erase( iterator _First, iterator _Last )
	{	// erase [_First, _Last)
		if (_First == begin() && _Last == end())
		{
			clear();
		}
		else
		{
			while (_First != _Last)
			{
				_First = erase(_First);
			}
		}
		return (_Last);
	}

	void remove(const Object& _Val)
	{	// erase each element matching _Val
		iterator _Last = end();
		for (iterator _First = begin(); _First != _Last; )
		{
			if (*_First == _Val)
			{
				_First = erase(_First);
			}
			else
			{
				++_First;
			}
		}
	}

	void swap(BCList<Object> &_RightList)
	{
		int nCount = _RightList.size();
		iterator _First = begin();
		iterator _Last = end();
		for (; _First != _Last; )
		{
			_RightList.push_back(*_First);
			_First = erase(_First);
		}
		_First = _RightList.begin();
		for (int i = 0;i < nCount;i++)
		{
			push_back(*_First);
			_First = _RightList.erase(_First);
		}
	}

	iterator find(const Object& _Val)
	{	// erase each element matching _Val
		iterator _Last = end();
		for (iterator _First = begin(); _First != _Last; )
		{
			if (*_First == _Val)
			{
				return _First;
			}
			else
			{
				++_First;
			}
		}
		return _Last;
	}

private:
	size_t				m_Size;
	Node				m_Head;
	Node				m_Tail;
	allocator			m_sAlloc;

	void init( )
	{
		m_Size = 0;
		m_Head.next = &m_Tail;
		m_Tail.prev = &m_Head;
	}
};


///////////////////////////////////////////////////////////////////////////////
// Namespace: BC
///////////////////////////////////////////////////////////////////////////////

} // End of namespace BC


#endif // BC_BCLIST_H_INCLUDED__

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
