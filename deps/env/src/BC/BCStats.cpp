///////////////////////////////////////////////////////////////////////////////
// file : BCStats.cpp
// author : antoniozhou
///////////////////////////////////////////////////////////////////////////////


#include <BC/Utils.h>
#include <BC/BCStats.h>
#include <BC/base/atomic_ref_count.h>

///////////////////////////////////////////////////////////////////////////////
// Namespace : BC
///////////////////////////////////////////////////////////////////////////////

namespace BC
{

///////////////////////////////////////////////////////////////////////////////
// Macro & typedefs
///////////////////////////////////////////////////////////////////////////////

#define BC_STATS_MAGIC			BC_MAGIC('S', 't', 'a', 't')
#define BC_STATS_VALID(x)		BC_MAGIC_VALID(x, BC_STATS_MAGIC)


///////////////////////////////////////////////////////////////////////////////
// class : BCStats
///////////////////////////////////////////////////////////////////////////////

IMPLEMENT_FIXED_ALLOC(BCStats, 2);

BCStats::BCStats()
	: BCMagic(0)
	, m_nCounters(0)
	, m_nRef(0)
	, m_pCounters(NULL)
	, m_pCopiedCounters(NULL)
{
	//
}

BCStats::~BCStats()
{
	//
}

BCRESULT BCStats::Create(int ncounters)
{
	BCRESULT result = BC_R_SUCCESS;

	m_pCounters = (BCStatType *)m_sPool.Calloc(sizeof(BCStatType)*ncounters);
	if (m_pCounters == NULL)
	{
		return BC_R_NOMEMORY;
	}

	m_pCopiedCounters = (uint64_t *)m_sPool.Calloc(sizeof(uint64_t)*ncounters);
	if (m_pCopiedCounters == NULL)
	{
		result = BC_R_NOMEMORY;
		goto out;
	}

#ifdef BC_RWLOCK_USEATOMIC
	result = m_sCounterRWLock.Create(0, 0);
	if (result != BC_R_SUCCESS)
		goto out;
#endif

	m_nRef = 1;
	memset(m_pCounters, 0, sizeof(BCStatType) * ncounters);
	m_nCounters = ncounters;
	m_nMagic = BC_STATS_MAGIC;

	return (result);

out:
	return result;
}

void BCStats::Attach(BCStats **statsp)
{
	ASSERT(BC_STATS_VALID(this));
	ASSERT(statsp != NULL && *statsp == NULL);

	this->m_sLock.Lock();
	this->m_nRef++;
	this->m_sLock.Unlock();

	*statsp = this;
}

void BCStats::Detach(BCStats **statsp)
{
	BCStats *stats;

	ASSERT(statsp != NULL && BC_STATS_VALID(*statsp));

	stats = *statsp;
	*statsp = NULL;

	stats->m_sLock.Lock();
	stats->m_nRef--;
	stats->m_sLock.Unlock();

	if (stats->m_nRef == 0)
	{
		BC_SAFE_DELETE_PTR(stats);
		*statsp = NULL;
	}
}

int BCStats::GetNumOfCounters()
{
	ASSERT(BC_STATS_VALID(this));

	return (this->m_nCounters);
}

void BCStats::Increment(int counter)
{
	int32_t prev;

	ASSERT(BC_STATS_VALID(this));
	ASSERT(counter < this->m_nCounters);

#ifdef BC_RWLOCK_USEATOMIC
	/*
	 * We use a "read" lock to prevent other threads from reading the
	 * counter while we "writing" a counter field.  The write access itself
	 * is protected by the atomic operation.
	 */
	this->m_sCounterRWLock.Lock(bc_rwlocktype_read);
#endif

#if BC_STATS_USEMULTIFIELDS
	prev = Base::AtomicRefCountIncN((bc_atomic_t *)&this->m_pCounters[counter].lo, 1);
	/*
	 * If the lower 32-bit field overflows, increment the higher field.
	 * Note that it's *theoretically* possible that the lower field
	 * overlaps again before the higher field is incremented.  It doesn't
	 * matter, however, because we don't read the value until
	 * BCStats::Copy() is called where the whole process is protected
	 * by the write (exclusive) lock.
	 */
	if (prev == (int32_t)0xffffffff)
		Base::AtomicRefCountIncN((bc_atomic_t *)&this->m_pCounters[counter].hi, 1);
#elif defined(BC_PLATFORM_HAVEXADDQ)
	UNUSED(prev);
	Base::AtomicRefCountIncN((int64_t *)&this->m_pCounters[counter], 1);
#else
	UNUSED(prev);
	this->m_pCounters[counter]++;
#endif

#ifdef BC_RWLOCK_USEATOMIC
	this->m_sCounterRWLock.Unlock(bc_rwlocktype_read);
#endif
}

void BCStats::Decrement(int counter)
{
	int32_t prev;

	ASSERT(BC_STATS_VALID(this));
	ASSERT(counter < this->m_nCounters);

#ifdef BC_RWLOCK_USEATOMIC
	this->m_sCounterRWLock.Lock(bc_rwlocktype_read);
#endif

#if BC_STATS_USEMULTIFIELDS
	prev = Base::AtomicRefCountIncN((bc_atomic_t *)&this->m_pCounters[counter].lo, -1);
	if (prev == 0)
		Base::AtomicRefCountIncN((bc_atomic_t *)&this->m_pCounters[counter].hi,	-1);
#elif defined(BC_PLATFORM_HAVEXADDQ)
	UNUSED(prev);
	Base::AtomicRefCountIncN((int64_t *)&this->m_pCounters[counter], -1);
#else
	UNUSED(prev);
	this->m_pCounters[counter]--;
#endif

#ifdef BC_RWLOCK_USEATOMIC
	this->m_sCounterRWLock.Unlock(bc_rwlocktype_read);
#endif
}

void BCStats::_CopyCounters()
{
	int i;

#ifdef BC_RWLOCK_USEATOMIC
	/*
	 * We use a "write" lock before "reading" the statistics counters as
	 * an exclusive lock.
	 */
	this->m_sCounterRWLock.Lock(bc_rwlocktype_write);
#endif

#if BC_STATS_USEMULTIFIELDS
	for (i = 0; i < this->m_nCounters; i++)
	{
		this->m_pCopiedCounters[i] =
				(uint64_t)(this->m_pCounters[i].hi) << 32 |
				this->m_pCounters[i].lo;
	}
#else
	UNUSED(i);
	memcpy(this->m_pCopiedCounters, this->m_pCounters,
	       this->m_nCounters * sizeof(BCStatType));
#endif

#ifdef BC_RWLOCK_USEATOMIC
	this->m_sCounterRWLock.Unlock(bc_rwlocktype_write);
#endif
}

void
BCStats::Dump(LPFN_BCStatsDumper dump_fn,
	       void *arg, unsigned int options)
{
	int i;

	ASSERT(BC_STATS_VALID(this));

	_CopyCounters();

	for (i = 0; i < this->m_nCounters; i++)
	{
		if ((options & BC_STATSDUMP_VERBOSE) == 0 &&
		    this->m_pCopiedCounters[i] == 0)
				continue;
		(dump_fn)((BCStatsCounterType)i, this->m_pCopiedCounters[i], arg);
	}
}

///////////////////////////////////////////////////////////////////////////////
// End of namespace : BC
///////////////////////////////////////////////////////////////////////////////

} // End of namespace BC

///////////////////////////////////////////////////////////////////////////////
// End of file : BCStats.cpp
///////////////////////////////////////////////////////////////////////////////
