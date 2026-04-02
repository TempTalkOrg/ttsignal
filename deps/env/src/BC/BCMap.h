
#ifndef BC_BCMAP_H_INCLUDED__
#define BC_BCMAP_H_INCLUDED__

#include "BC/Exports.h"
#include "BC/Utils.h"
#include "BC/ext/hash_fun.h"

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

///////////////////////////////////////////////////////////////////////////////
// Namespace: BC
///////////////////////////////////////////////////////////////////////////////

namespace BC
{

///////////////////////////////////////////////////////////////////////////////
// BCMap
///////////////////////////////////////////////////////////////////////////////

//#define SIZE_TO_SHIFT(shift) ((shift) < 0 ? 0: 1<<(shift))
#define REHASH(v, size) (( (37 * (v) / 7 + 5 ) & ((size) - 1)) | 1)
#define INITIAL_SHIFT 3
#define INITIAL_SIZE 8

typedef enum cell_status
{
	ILL			= -1,
	VACANT		= 0,
	OCCUPIED	= 1,
	REMOVED		= 2,
} cell_status;

template <typename _Key, typename _Value>
class BCMap
{
public:
	typedef struct pair_t
	{
		_Key			first;
		_Value			second;
	} pair_t;

	typedef struct cell_t
	{
		long		hash;
		pair_t		pair;
	} cell_t;

	class const_iterator
	{
	protected:
		BCMap<_Key, _Value>		*	m_pDict;
		int32_t						m_index;
		pair_t						m_tmpPair;
	public:
		const_iterator(BCMap<_Key, _Value> * d = NULL, int32_t idx=0)
		{
			m_pDict = d;
			m_index = idx;
		}

		void update()
		{
		}

		const const_iterator& next()
		{
			m_index++;
			for (;m_index < SIZE_TO_SHIFT(m_pDict->m_nSizeShift); m_index++)
			{
				if (m_pDict->getStatus(m_pDict->m_pArrayStatus, m_index) == OCCUPIED)
				{
					update();
					return *this;
				}
			}
			return const_iterator();
		}

		const pair_t & operator*()
		{
			if (!m_pDict)
			{
				return m_tmpPair;
			}
			for (;m_index < SIZE_TO_SHIFT(m_pDict->m_nSizeShift); m_index++)
			{
				if (m_pDict->getStatus(m_pDict->m_pArrayStatus, m_index) == OCCUPIED)
				{
					return m_pDict->m_pCells[m_index].pair;
				}
			}
			return m_tmpPair;
		}

		const pair_t * operator->()
		{
			if (!m_pDict)
			{
				return &m_tmpPair;
			}
			for (;m_index < SIZE_TO_SHIFT(m_pDict->m_nSizeShift); m_index++)
			{
				if (m_pDict->getStatus(m_pDict->m_pArrayStatus, m_index) == OCCUPIED)
				{
					return &m_pDict->m_pCells[m_index].pair;
				}
			}
			return &m_tmpPair;
		}

		const_iterator & operator=(const const_iterator &o)
		{
			m_pDict = o.m_pDict;
			m_index = o.m_index;
			return *this;
		}

		const_iterator & operator++(int32_t idx)
		{
			if (!m_pDict)
			{
				return *this;
			}
			m_index++;
			for (;m_index < SIZE_TO_SHIFT(m_pDict->m_nSizeShift); m_index++)
			{
				if (m_pDict->getStatus(m_pDict->m_pArrayStatus, m_index) == OCCUPIED)
				{
					update();
					return *this;
				}
			}
			m_pDict = NULL;
			m_index = 0;
			return *this;
		}

		bool operator==(const const_iterator &other) const
		{
			return (m_pDict == NULL && other.m_pDict == NULL) ||
				(m_pDict == other.m_pDict && m_index == other.m_index);
		}

		bool operator !=(const const_iterator &other) const
		{
			return !(*this == other);
		}
	};

	//typedef  * iterator;value
	class iterator : public const_iterator
	{
		using		const_iterator::m_index;
		using		const_iterator::m_pDict;
		using		const_iterator::m_tmpPair;
	public:

		iterator(BCMap<_Key, _Value> * d = NULL, int32_t idx=0)
			: const_iterator(d, idx)
		{
			//
		}

		void update()
		{
		}

		iterator& next()
		{
			m_index++;
			for (;m_index < SIZE_TO_SHIFT(m_pDict->m_nSizeShift); m_index++)
			{
				if (m_pDict->getStatus(m_pDict->m_pArrayStatus, m_index) == OCCUPIED)
				{
					update();
					return *this;
				}
			}
			return iterator();
		}

		pair_t & operator*()
		{
			if (!m_pDict)
			{
				return m_tmpPair;
			}
			for (;m_index < SIZE_TO_SHIFT(m_pDict->m_nSizeShift); m_index++)
			{
				if (m_pDict->getStatus(m_pDict->m_pArrayStatus, m_index) == OCCUPIED)
				{
					return m_pDict->m_pCells[m_index].pair;
				}
			}
			return m_tmpPair;
		}

		pair_t * operator->()
		{
			if (!m_pDict)
			{
				return &m_tmpPair;
			}
			for (;m_index < SIZE_TO_SHIFT(m_pDict->m_nSizeShift); m_index++)
			{
				if (m_pDict->getStatus(m_pDict->m_pArrayStatus, m_index) == OCCUPIED)
				{
					return &m_pDict->m_pCells[m_index].pair;
				}
			}
			return &m_tmpPair;
		}

		iterator & operator=(iterator o)
		{
			m_pDict = o.m_pDict;
			m_index = o.m_index;
			return *this;
		}

		iterator & operator++(int32_t idx)
		{
			UNUSED(idx);
			if (!m_pDict)
			{
				return *this;
			}
			m_index++;
			for (;m_index < SIZE_TO_SHIFT(m_pDict->m_nSizeShift); m_index++)
			{
				if (m_pDict->getStatus(m_pDict->m_pArrayStatus, m_index) == OCCUPIED)
				{
					update();
					return *this;
				}
			}
			m_pDict = NULL;
			m_index = 0;
			return *this;
		}

		bool operator==(iterator  other)
		{
			return (m_pDict == NULL && other.m_pDict == NULL) ||
			       (m_pDict == other.m_pDict && m_index == other.m_index);
		}

		bool operator !=(iterator  other)
		{
			return !(*this == other);
		}

	};

private:
	int32_t				m_nSizeShift;
	uint8_t			*	m_pArrayStatus;
	int32_t				m_cell_count;
	int32_t				m_cell_removed;
	cell_t			*	m_pCells;

public:
	BCMap()
	{
		m_cell_count = m_cell_removed = 0;
		m_nSizeShift = INITIAL_SHIFT;
		int32_t size = SIZE_TO_SHIFT(m_nSizeShift);

		m_pArrayStatus = (uint8_t *)calloc(SIZE_TO_SHIFT(m_nSizeShift - 2), 1);
		m_pCells = (cell_t *)new cell_t[size];
		tmpV = _Value();
	}

	~BCMap()
	{
		if (m_pArrayStatus)
		{
			free(m_pArrayStatus);
		}
		BC_SAFE_DELETE_ARRAY(m_pCells);
	}

	// attributes operation
public:
	int32_t size_shift() const { return this->m_nSizeShift; };
	uint8_t *arr_status()const { return this->m_pArrayStatus; };
	int32_t cell_count() const { return this->m_cell_count; };
	cell_t * cells() const { return this->m_pCells; };
	int32_t size_after_shift() const { return SIZE_TO_SHIFT(this->m_nSizeShift); };

public:
	cell_status getStatus(uint8_t * arr_status, int32_t index)
	{
		uint8_t mask = 0x3 <<((index & 0x3)<<1);
		return (cell_status)((mask & arr_status[index >> 2]) >> ((index & 0x3)<<1));
	}

	int32_t setStatus(uint8_t * arr_status, int32_t index, cell_status newstatus)
	{
		uint8_t mask = ~(3 <<((index & 0x3)<<1));
		uint8_t newmask = (newstatus & 3)<<((index & 3)<<1);
		arr_status[index >> 2] = (arr_status[index >> 2] & mask) | newmask;
		return newstatus;
	}

	int32_t size() const
	{
		return m_cell_count;
	}

	cell_status putCell(cell_t * cells, uint8_t * arr_status, int32_t size, long hash, _Key key, _Value value)
	{
		__gnu_cxx::hash<_Key> h;
		long ahash = (long)h(key);

		/* first rount hasing */
		long hash1 = ahash & (size - 1);
		cell_t * pcell = cells + hash1;

		cell_status status  = getStatus(arr_status, hash1);

		UNUSED(hash);

		if (status == VACANT || status == REMOVED)
		{
			pcell->hash = ahash;
			pcell->pair.first = key;
			pcell->pair.second = value;
			cell_status oldstatus = status;

			setStatus(arr_status, hash1, OCCUPIED);
			return oldstatus;
		}
		else if (pcell->pair.first == key)   // Replace the current cell
		{
			pcell->pair.second = value;
			pcell->hash = ahash;
			return OCCUPIED;
		}
		else  // Cell occupied
		{
			/* Rehasing */
			long hash2 = REHASH(ahash, size);   // rehashing
			int32_t i;
			long thash = hash1;                // Oraginal place

			for (i = 0; i < size; i++)    //
			{
				thash  -= hash2;
				if (thash < 0)
				{
					thash += size;  // Next cell
				}

				pcell = cells + thash;

				status  = getStatus(arr_status, thash);
				if (status == VACANT || status == REMOVED)   // take a vacant cell
				{
					pcell->hash = ahash;
					pcell->pair.first = key;
					pcell->pair.second = value;

					cell_status oldstatus = status;

					setStatus(arr_status, thash, OCCUPIED);
					return oldstatus;

				}
				else if (pcell->pair.first == key)          // Replace an existing cell
				{
					pcell->pair.second = value;
					pcell->hash = ahash;
					return OCCUPIED;
				}
			} // End for
		}
		return ILL;
	}

	int32_t findIndex(_Key key)
	{
		__gnu_cxx::hash<_Key> h;
		long ahash = (long)h(key);

		int32_t size = SIZE_TO_SHIFT(this->m_nSizeShift);
		long hash1 = ahash & (size - 1);
		int32_t i;

		cell_t * pcell = this->m_pCells + hash1;

		cell_status status  = getStatus(this->m_pArrayStatus, hash1);

		if (status == OCCUPIED && (zcmp(pcell->pair.first, key) == 0))
		{
			return hash1;
		}
		else
		{
			long hash2 = REHASH(ahash, size); /* Rehash */
			long thash = hash1;		     /*  */

			for (i = 0; i< size; i++)
			{
				thash -= hash2;

				if (thash < 0)
				{
					thash += size;   // Loop back
				}

				pcell = this->m_pCells + thash;  // Point to the current cell

				status  = getStatus(this->m_pArrayStatus, thash);
				if (status == OCCUPIED && (zcmp(pcell->pair.first, key)== 0))
				{
					return thash;                // Found
				}
				else if (status == VACANT)
				{
					return -1;                   // Abandon searching on vacant cell
				}
			}
		}// End for
		return -2;   // Cannot go here
	}

	int32_t remove(_Key key)
	{
		//__gnu_cxx::hash<_Key> h;
		//long ahash = h(key);
		int32_t idx = findIndex(key);
		if (idx >= 0)
		{
			setStatus(this->m_pArrayStatus, idx, REMOVED);

			this->m_cell_removed++;
			this->m_cell_count--;

			if (this->m_cell_count < (int32_t)(0.25 * SIZE_TO_SHIFT(this->m_nSizeShift)))
			{
				resize( this->m_nSizeShift - 1);
			}
		}
		return 0;
	}

	void clear()
	{
		m_cell_count = m_cell_removed = 0;
		m_nSizeShift = INITIAL_SHIFT;
		int32_t size = SIZE_TO_SHIFT(m_nSizeShift);

		if (m_pArrayStatus)
		{
			free(m_pArrayStatus);
			m_pArrayStatus = NULL;
		}
		BC_SAFE_DELETE_ARRAY(m_pCells);
		m_pArrayStatus = (uint8_t *)calloc(SIZE_TO_SHIFT(m_nSizeShift - 2), 1);
		m_pCells = (cell_t *)new cell_t[size];
	}

	int32_t put(_Key key, _Value value)
	{
		__gnu_cxx::hash<_Key> h;
		long ahash = (long)h(key);

		if (this->m_cell_count > (int32_t)(0.8 * SIZE_TO_SHIFT(this->m_nSizeShift)))   // Too satiated
		{
			resize( this->m_nSizeShift + 1);
		}

		cell_status ret = putCell(this->m_pCells, this->m_pArrayStatus, SIZE_TO_SHIFT(this->m_nSizeShift), ahash, key, value);


		if (ret < 0)
		{
			resize( this->m_nSizeShift + 1);
			ret = putCell(this->m_pCells, this->m_pArrayStatus, SIZE_TO_SHIFT(this->m_nSizeShift), ahash, key, value);
			assert(ret != REMOVED);
			assert(this->m_cell_removed == 0);
		}
		else if (ret == REMOVED)
		{
			this->m_cell_removed--;                   // elem takes a removed cell
		}

		if (ret != OCCUPIED)                       // replace into the m_pDict instead of occuping a new cell
		{
			this->m_cell_count++;
		}

		return this->m_cell_count;
	}

	int32_t resize(int32_t new_shift)
	{

		int32_t i = 0;
		cell_t * pcell;
		int32_t newsize = SIZE_TO_SHIFT(new_shift);

		if (newsize < this->m_cell_count )
		{
			return 0;
		}

		if (newsize < INITIAL_SIZE)
		{
			newsize = INITIAL_SIZE;
			new_shift = INITIAL_SHIFT;
			return 0;
		}

		int32_t size = SIZE_TO_SHIFT(this->m_nSizeShift);
		uint8_t * arr_status;
		cell_t * newcells;

		arr_status = (uint8_t *)calloc(SIZE_TO_SHIFT(new_shift - 2), 1);
		newcells = (cell_t *)new cell_t[newsize];

		int32_t occupied_count = 0;
		for (i=0; i< size; i++)
		{
			pcell = this->m_pCells + i;
			cell_status status = getStatus(this->m_pArrayStatus, i);

			if (status == OCCUPIED)
			{
				putCell(newcells, arr_status, newsize, pcell->hash, pcell->pair.first, pcell->pair.second);
				occupied_count++;
			}
		}

		if (occupied_count != this->m_cell_count)
		{
			throw BC_R_RANGE;
		}

		BC_SAFE_DELETE_ARRAY(m_pCells);
		this->m_pCells = newcells;
		this->m_nSizeShift = new_shift;
		this->m_cell_removed = 0;
		if (m_pArrayStatus)
		{
			free(m_pArrayStatus);
		}
		this->m_pArrayStatus = arr_status;

		//printf("Changed to new size %d\n", SIZE_TO_SHIFT(this->m_nSizeShift));
		/* keep this->m_cell_count  */
		return newsize;
	}

	_Value tmpV;
	//iterator& operator++(iterator& it, int32_t)

	iterator begin()
	{
		int32_t i = 0;
		for (i = 0; i < SIZE_TO_SHIFT(m_nSizeShift); i++)
		{
			if (getStatus(m_pArrayStatus, i) == OCCUPIED)
			{
				iterator it =  iterator(this, i);
				//it.update();
				return it;
			}
		}
		return end();
	}

	iterator end()
	{
		//return iterator(this, SIZE_TO_SHIFT(m_nSizeShift));
		return iterator();
	}

	_Value & get(_Key key)
	{
		int32_t idx = findIndex(key);
		if (idx < 0)
		{
			uint8_t b[sizeof(_Value)] = {0};
			_Value *pVal = (_Value *)b;

			put(key, initKs(*pVal));

			idx = findIndex(key);
		}

		if (idx >= 0)
		{
			return m_pCells[idx].pair.second;
		}
		else
		{
			return tmpV;
		}
	}

	_Value & operator[](_Key key)
	{
		return get(key);
	}

	bool hasKey(_Key key)
	{
		int32_t idx = findIndex(key);
		return idx >= 0;
	}

	iterator find(_Key key)
	{
		int32_t idx = findIndex(key);
		if (idx >= 0)
		{
			return iterator(this, idx);
		}
		else
		{
			return end();
		}
	}

	void erase(iterator &iter)
	{
		remove(iter->first);
	}

	void erase(_Key key)
	{
		iterator iter = find(key);
		if (iter != end())
		{
			erase(iter);
		}
	}
};

///////////////////////////////////////////////////////////////////////////////
// Namespace: BC
///////////////////////////////////////////////////////////////////////////////

} // End of namespace BC


#endif // BC_BCMAP_H_INCLUDED__

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
