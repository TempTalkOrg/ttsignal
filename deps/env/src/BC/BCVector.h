
#ifndef BC_BCVECTOR_H_INCLUDED__
#define BC_BCVECTOR_H_INCLUDED__

#include <BC/Exports.h>
#include <BC/Utils.h>

///////////////////////////////////////////////////////////////////////////////
// Namespace: BC
///////////////////////////////////////////////////////////////////////////////

namespace BC
{

///////////////////////////////////////////////////////////////////////////////
// TPoolVector
///////////////////////////////////////////////////////////////////////////////

template <typename _ValueType>
class BCVector
{
private:
	_ValueType		*	m_pCells;
	int32_t				m_nSizeShift;
	int32_t				m_nCellCount;
	_ValueType			m_sTempV;

public:
	BCVector()
	{
		uint32_t nSize;

		m_nSizeShift = 2;
		nSize = SIZE_TO_SHIFT(m_nSizeShift);
		m_pCells = new _ValueType[nSize];
		m_nCellCount = 0;
	}

	virtual ~BCVector()
	{
		BC_SAFE_DELETE_ARRAY(m_pCells);
	}

	void clear()
	{
		BC_SAFE_DELETE_ARRAY(m_pCells);
		m_nSizeShift = 2;
		uint32_t nSize = SIZE_TO_SHIFT(m_nSizeShift);
		m_pCells = new _ValueType[nSize];
		m_nCellCount = 0;
	}

	void resize(int32_t newsize)
	{
		int32_t nSizeShift = m_nSizeShift;
		int32_t nCurrSize = m_nSizeShift;

		while (SIZE_TO_SHIFT(nSizeShift) < newsize)
		{
			nSizeShift++;
		}

		int32_t nNewCellSize = SIZE_TO_SHIFT(nSizeShift);
		if (nSizeShift >= nCurrSize)
		{
			_ValueType * pNewCells ;

			pNewCells = new _ValueType[nNewCellSize];
			for (int32_t i = 0; i < m_nCellCount; i++)
			{
				pNewCells[i] = m_pCells[i];
			}
			BC_SAFE_DELETE_ARRAY(m_pCells);
			m_nSizeShift = nSizeShift;
			m_pCells = pNewCells;
		}
	}


	int32_t size() const
	{
		return m_nCellCount;
	}

	_ValueType& operator[](int32_t k)
	{
		if (k >=0 && k < m_nCellCount)
		{
			return m_pCells[k];
		}
		else if (k>=m_nCellCount)
		{
			resize(k+1);
			m_nCellCount = k + 1;
			return m_pCells[k];
		}
		return m_sTempV;
	}

	void operator+=(_ValueType v)
	{
		push_back(v);
	}

	void push_back(_ValueType v)
	{
		if (m_nCellCount >= SIZE_TO_SHIFT(m_nSizeShift))
		{
			resize(m_nCellCount + 1);
		}
		m_pCells[m_nCellCount++] = v;
	}
	BCVector& operator<<(_ValueType v)
	{
		push_back(v);
		return *this;
	}

	///////////////////////////////////////////////////////////////////////////////
	// class : BCPVector::interator
	///////////////////////////////////////////////////////////////////////////////
	class iterator
	{
	public:
		BCVector	*	m_pVector;
		int32_t			m_nIndex;

		iterator(BCVector * v = NULL, int32_t idx = 0)
		{
			m_pVector = v;
			m_nIndex = idx;
			update();
		}

		void update()
		{
		}

		_ValueType & operator*()
		{
			return (*m_pVector)[m_nIndex];
		}

		_ValueType const& operator* () const
		{
			return (*m_pVector)[m_nIndex];
		}


		iterator & operator=(iterator o)
		{
			m_pVector = o.m_pVector;
			m_nIndex = o.m_nIndex;
			update();
			return *this;
		}

		iterator & operator++(int32_t)
		{
			if (m_pVector)
			{
				if (m_nIndex < m_pVector->size() - 1)
				{
					m_nIndex++;
					update();
				}
				else
				{
					m_pVector = 0;
					m_nIndex = 0;
				}
			}
			return *this;
		}

		bool operator==(iterator o)
		{
			return (m_pVector == o.m_pVector && m_nIndex == o.m_nIndex);
		}
		bool operator!=(iterator o)
		{
			return (m_pVector != o.m_pVector|| m_nIndex != o.m_nIndex);
		}
	};

	//friend class iterator;

	iterator begin()
	{
		if (m_nCellCount > 0)
		{
			return iterator(this, 0);
		}
		else
		{
			return end();
		}
	}

	iterator end()
	{
		return iterator();
	}
};

///////////////////////////////////////////////////////////////////////////////
// Namespace: BC
///////////////////////////////////////////////////////////////////////////////

} // End of namespace BC


#endif // BC_BCVECTOR_H_INCLUDED__

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
