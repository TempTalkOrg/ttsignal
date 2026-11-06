
#include <BC/Utils.h>
#include <BC/BCRWLock.h>
#include <BC/base/atomic_ref_count.h>


///////////////////////////////////////////////////////////////////////////////
// Namespace : BC
///////////////////////////////////////////////////////////////////////////////

namespace BC
{

#define RWLOCK_MAGIC		BC_MAGIC('R', 'W', 'L', 'k')
#define VALID_RWLOCK(rwl)	BC_MAGIC_VALID(rwl, RWLOCK_MAGIC)

#ifdef BC_PLATFORM_USETHREADS

#ifndef RWLOCK_DEFAULT_READ_QUOTA
#define RWLOCK_DEFAULT_READ_QUOTA 4
#endif

#ifndef RWLOCK_DEFAULT_WRITE_QUOTA
#define RWLOCK_DEFAULT_WRITE_QUOTA 4
#endif

#define ATOMIC_INCN(ptr, n)	(Base::AtomicRefCountIncN(ptr, n) - n)

///////////////////////////////////////////////////////////////////////////////
// class : BCRWLock
///////////////////////////////////////////////////////////////////////////////

BCRWLock::BCRWLock()
	: BCMagic(RWLOCK_MAGIC)
	, m_sLock()
#if defined(BC_PLATFORM_HAVEXADD) && defined(BC_PLATFORM_HAVECMPXCHG)
	, m_nWriteRequests(0)
	, m_nWriteCompletions(0)
	, m_nCntAndFlag(0)
	, m_sCondReadable(&m_sLock)
	, m_sCondWriteable(&m_sLock)
	, m_nReadersWaiting(0)
	, m_nWriteGranted(0)
	, m_nWriteQuota(RWLOCK_DEFAULT_WRITE_QUOTA)
#else
	, m_sCondReadable(&m_sLock)
	, m_sCondWriteable(&m_sLock)
	, m_eType(bc_rwlocktype_read)
	, m_nActive(0)
	, m_nGranted(0)
	, m_nReadersWaiting(0)
	, m_nWritersWaiting(0)
	, m_nReadQuota(RWLOCK_DEFAULT_READ_QUOTA)
	, m_nWriteQuota(RWLOCK_DEFAULT_WRITE_QUOTA)
	, m_eOriginal(bc_rwlocktype_none)
#endif
{
	//
}

BCRWLock::~BCRWLock()
{
	//
}

BCRESULT
BCRWLock::Create(unsigned int read_quota, unsigned int write_quota)
{
	UNUSED(read_quota);
	ASSERT(this != NULL);

	/*
	 * In case there's trouble initializing, we zero magic now.  If all
	 * goes well, we'll set it to RWLOCK_MAGIC.
	 */
	this->m_nMagic = 0;

#if defined(BC_PLATFORM_HAVEXADD) && defined(BC_PLATFORM_HAVECMPXCHG)
	this->m_nWriteRequests = 0;
	this->m_nWriteCompletions = 0;
	this->m_nCntAndFlag = 0;
	this->m_nReadersWaiting = 0;
	this->m_nWriteGranted = 0;
	if (write_quota == 0)
		write_quota = RWLOCK_DEFAULT_WRITE_QUOTA;
	this->m_nWriteQuota = write_quota;
#else
	this->m_eType = bc_rwlocktype_read;
	this->m_eOriginal = bc_rwlocktype_none;
	this->m_nActive = 0;
	this->m_nGranted = 0;
	this->m_nReadersWaiting = 0;
	this->m_nWritersWaiting = 0;
	if (read_quota == 0)
		read_quota = RWLOCK_DEFAULT_READ_QUOTA;
	this->m_nReadQuota = read_quota;
	if (write_quota == 0)
		write_quota = RWLOCK_DEFAULT_WRITE_QUOTA;
	this->m_nWriteQuota = write_quota;
#endif

	this->m_nMagic = RWLOCK_MAGIC;

	return (BC_R_SUCCESS);
}

void
BCRWLock::Destroy()
{
	ASSERT(VALID_RWLOCK(this));

#if defined(BC_PLATFORM_HAVEXADD) && defined(BC_PLATFORM_HAVECMPXCHG)
	ASSERT(this->m_nWriteRequests == this->m_nWriteCompletions &&
		this->m_nCntAndFlag == 0 && this->m_nReadersWaiting == 0);
#else
	m_sLock.Lock();
	ASSERT(this->m_nActive == 0 &&
		this->m_nReadersWaiting == 0 &&
		this->m_nWritersWaiting == 0);
	m_sLock.Unlock();
#endif

	this->m_nMagic = 0;
}

#if defined(BC_PLATFORM_HAVEXADD) && defined(BC_PLATFORM_HAVECMPXCHG)

/*
 * When some architecture-dependent atomic operations are available,
 * rwlock can be more efficient than the generic algorithm defined below.
 * The basic algorithm is described in the following URL:
 *   http://www.cs.rochester.edu/u/scott/synchronization/pseudocode/rw.html
 *
 * The key is to use the following integer variables modified atomically:
 *   write_requests, write_completions, and cnt_and_flag.
 *
 * write_requests and write_completions act as a waiting queue for writers
 * in order to ensure the FIFO order.  Both variables begin with the initial
 * value of 0.  When a new writer tries to get a write lock, it increments
 * write_requests and gets the previous value of the variable as a "ticket".
 * When write_completions reaches the ticket number, the new writer can start
 * writing.  When the writer completes its work, it increments
 * write_completions so that another new writer can start working.  If the
 * write_requests is not equal to write_completions, it means a writer is now
 * working or waiting.  In this case, a new readers cannot start reading, or
 * in other words, this algorithm basically prefers writers.
 *
 * cnt_and_flag is a "lock" shared by all readers and writers.  This integer
 * variable is a kind of structure with two members: writer_flag (1 bit) and
 * reader_count (31 bits).  The writer_flag shows whether a writer is working,
 * and the reader_count shows the number of readers currently working or almost
 * ready for working.  A writer who has the current "ticket" tries to get the
 * lock by exclusively setting the writer_flag to 1, provided that the whole
 * 32-bit is 0 (meaning no readers or writers working).  On the other hand,
 * a new reader tries to increment the "reader_count" field provided that
 * the writer_flag is 0 (meaning there is no writer working).
 *
 * If some of the above operations fail, the reader or the writer sleeps
 * until the related condition changes.  When a working reader or writer
 * completes its work, some readers or writers are sleeping, and the condition
 * that suspended the reader or writer has changed, it wakes up the sleeping
 * readers or writers.
 *
 * As already noted, this algorithm basically prefers writers.  In order to
 * prevent readers from starving, however, the algorithm also introduces the
 * "writer quota" (Q).  When Q consecutive writers have completed their work,
 * suspending readers, the last writer will wake up the readers, even if a new
 * writer is waiting.
 *
 * Implementation specific note: due to the combination of atomic operations
 * and a mutex lock, ordering between the atomic operation and locks can be
 * very sensitive in some cases.  In particular, it is generally very important
 * to check the atomic variable that requires a reader or writer to sleep after
 * locking the mutex and before actually sleeping; otherwise, it could be very
 * likely to cause a deadlock.  For example, assume "var" is a variable
 * atomically modified, then the corresponding code would be:
 *	if (var == need_sleep) {
 *		LOCK(lock);
 *		if (var == need_sleep)
 *			WAIT(cond, lock);
 *		UNLOCK(lock);
 *	}
 * The second check is important, since "var" is protected by the atomic
 * operation, not by the mutex, and can be changed just before sleeping.
 * (The first "if" could be omitted, but this is also important in order to
 * make the code efficient by avoiding the use of the mutex unless it is
 * really necessary.)
 */

#define WRITER_ACTIVE	0x1
#define READER_INCR		0x2

BCRESULT
BCRWLock::Lock(BCRWLockTypeE type)
{
	int32_t cntflag;

	ASSERT(VALID_RWLOCK(this));

	if (type == bc_rwlocktype_read)
	{
		if (this->m_nWriteRequests != this->m_nWriteCompletions)
		{
			/* there is a waiting or active writer */
			m_sLock.Lock();
			if (this->m_nWriteRequests != this->m_nWriteCompletions)
			{
				this->m_nReadersWaiting++;
				this->m_sCondReadable.Wait();
				this->m_nReadersWaiting--;
			}
			m_sLock.Unlock();
		}

		cntflag = ATOMIC_INCN(&this->m_nCntAndFlag, READER_INCR);
		while (1) {
			if ((this->m_nCntAndFlag & WRITER_ACTIVE) == 0)
				break;

			/* A writer is still working */
			m_sLock.Lock();
			this->m_nReadersWaiting++;
			if ((this->m_nCntAndFlag & WRITER_ACTIVE) != 0)
				this->m_sCondReadable.Wait();
			this->m_nReadersWaiting--;
			m_sLock.Unlock();

			/*
			 * Typically, the reader should be able to get a lock
			 * at this stage:
			 *   (1) there should have been no pending writer when
			 *       the reader was trying to increment the
			 *       counter; otherwise, the writer should be in
			 *       the waiting queue, preventing the reader from
			 *       proceeding to this point.
			 *   (2) once the reader increments the counter, no
			 *       more writer can get a lock.
			 * Still, it is possible another writer can work at
			 * this point, e.g. in the following scenario:
			 *   A previous writer unlocks the writer lock.
			 *   This reader proceeds to point (1).
			 *   A new writer appears, and gets a new lock before
			 *   the reader increments the counter.
			 *   The reader then increments the counter.
			 *   The previous writer notices there is a waiting
			 *   reader who is almost ready, and wakes it up.
			 * So, the reader needs to confirm whether it can now
			 * read explicitly (thus we loop).  Note that this is
			 * not an infinite process, since the reader has
			 * incremented the counter at this point.
			 */
		}

		/*
		 * If we are temporarily preferred to writers due to the writer
		 * quota, reset the condition (race among readers doesn't
		 * matter).
		 */
		this->m_nWriteGranted = 0;
	}
	else
	{
		int32_t prev_writer;

		/* enter the waiting queue, and wait for our turn */
		prev_writer = ATOMIC_INCN(&this->m_nWriteRequests, 1);
		while (this->m_nWriteCompletions != prev_writer)
		{
			m_sLock.Lock();
			if (this->m_nWriteCompletions != prev_writer)
			{
				this->m_sCondWriteable.Wait();
				m_sLock.Unlock();
				continue;
			}
			m_sLock.Unlock();
			break;
		}

		while (1)
		{
			cntflag = Base::AtomicCompareAndSwap(&this->m_nCntAndFlag, 0,
						     WRITER_ACTIVE);
			if (cntflag == 0)
				break;

			/* Another active reader or writer is working. */
			m_sLock.Lock();
			if (this->m_nCntAndFlag != 0)
				this->m_sCondWriteable.Wait();
			m_sLock.Unlock();
		}

		ASSERT((this->m_nCntAndFlag & WRITER_ACTIVE) != 0);
		this->m_nWriteGranted++;
	}

	return (BC_R_SUCCESS);
}

BCRESULT
BCRWLock::TryLock(BCRWLockTypeE type)
{
	int32_t cntflag;

	ASSERT(VALID_RWLOCK(this));

	if (type == bc_rwlocktype_read)
	{
		/* If a writer is waiting or working, we fail. */
		if (this->m_nWriteRequests != this->m_nWriteCompletions)
			return (BC_R_LOCKBUSY);

		/* Otherwise, be ready for reading. */
		cntflag = ATOMIC_INCN(&this->m_nCntAndFlag, READER_INCR);
		if ((cntflag & WRITER_ACTIVE) != 0)
		{
			/*
			 * A writer is working.  We lose, and cancel the read
			 * request.
			 */
			cntflag = ATOMIC_INCN(&this->m_nCntAndFlag,
						  -READER_INCR);
			/*
			 * If no other readers are waiting and we've suspended
			 * new writers in this short period, wake them up.
			 */
			if (cntflag == READER_INCR &&
			    this->m_nWriteCompletions != this->m_nWriteRequests)
			{
				m_sLock.Lock();
				this->m_sCondWriteable.Broadcast();
				m_sLock.Unlock();
			}

			return (BC_R_LOCKBUSY);
		}
	}
	else
	{
		/* Try locking without entering the waiting queue. */
		cntflag = Base::AtomicCompareAndSwap(&this->m_nCntAndFlag, 0, WRITER_ACTIVE);
		if (cntflag != 0)
			return (BC_R_LOCKBUSY);

		/*
		 * XXXJT: jump into the queue, possibly breaking the writer
		 * order.
		 */
		(void)ATOMIC_INCN(&this->m_nWriteCompletions, -1);

		this->m_nWriteGranted++;
	}

	return (BC_R_SUCCESS);
}

BCRESULT
BCRWLock::TryUpgrade()
{
	int32_t prevcnt;

	ASSERT(VALID_RWLOCK(this));

	/* Try to acquire write access. */
	prevcnt = Base::AtomicCompareAndSwap(&this->m_nCntAndFlag,
				     READER_INCR, WRITER_ACTIVE);
	/*
	 * There must have been no writer, and there must have been at least
	 * one reader.
	 */
	ASSERT((prevcnt & WRITER_ACTIVE) == 0 &&
	       (prevcnt & ~WRITER_ACTIVE) != 0);

	if (prevcnt == READER_INCR)
	{
		/*
		 * We are the only reader and have been upgraded.
		 * Now jump into the head of the writer waiting queue.
		 */
		(void)ATOMIC_INCN(&this->m_nWriteCompletions, -1);
	}
	else
		return (BC_R_LOCKBUSY);

	return (BC_R_SUCCESS);

}

void
BCRWLock::Downgrade()
{
	int32_t prev_readers;

	ASSERT(VALID_RWLOCK(this));

	/* Become an active reader. */
	prev_readers = ATOMIC_INCN(&this->m_nCntAndFlag, READER_INCR);
	/* We must have been a writer. */
	ASSERT((prev_readers & WRITER_ACTIVE) != 0);

	/* Complete write */
	(void)ATOMIC_INCN(&this->m_nCntAndFlag, -WRITER_ACTIVE);
	(void)ATOMIC_INCN(&this->m_nWriteCompletions, 1);

	/* Resume other readers */
	m_sLock.Lock();
	if (this->m_nReadersWaiting > 0)
		this->m_sCondReadable.Broadcast();
	m_sLock.Unlock();
}

BCRESULT
BCRWLock::Unlock(BCRWLockTypeE type)
{
	int32_t prev_cnt;

	ASSERT(VALID_RWLOCK(this));

	if (type == bc_rwlocktype_read)
	{
		prev_cnt = ATOMIC_INCN(&this->m_nCntAndFlag, -READER_INCR);

		/*
		 * If we're the last reader and any writers are waiting, wake
		 * them up.  We need to wake up all of them to ensure the
		 * FIFO order.
		 */
		if (prev_cnt == READER_INCR &&
		    this->m_nWriteCompletions != this->m_nWriteRequests)
		{
			m_sLock.Lock();
			this->m_sCondWriteable.Broadcast();
			m_sLock.Unlock();
		}
	}
	else
	{
		BOOL wakeup_writers = TRUE;

		/*
		 * Reset the flag, and (implicitly) tell other writers
		 * we are done.
		 */
		(void)ATOMIC_INCN(&this->m_nCntAndFlag, -WRITER_ACTIVE);
		(void)ATOMIC_INCN(&this->m_nWriteCompletions, 1);

		if (this->m_nWriteGranted >= this->m_nWriteQuota ||
		    this->m_nWriteRequests == this->m_nWriteCompletions ||
		    (this->m_nCntAndFlag & ~WRITER_ACTIVE) != 0)
		{
			/*
			 * We have passed the write quota, no writer is
			 * waiting, or some readers are almost ready, pending
			 * possible writers.  Note that the last case can
			 * happen even if write_requests != write_completions
			 * (which means a new writer in the queue), so we need
			 * to catch the case explicitly.
			 */
			m_sLock.Lock();
			if (this->m_nReadersWaiting > 0)
			{
				wakeup_writers = FALSE;
				this->m_sCondReadable.Broadcast();
			}
			m_sLock.Unlock();
		}

		if (this->m_nWriteRequests != this->m_nWriteCompletions &&
		    wakeup_writers)
		{
			m_sLock.Lock();
			this->m_sCondWriteable.Broadcast();
			m_sLock.Unlock();
		}
	}

	return (BC_R_SUCCESS);
}

#else /* BC_PLATFORM_HAVEXADD && BC_PLATFORM_HAVECMPXCHG */

BCRESULT
BCRWLock::_doit(BCRWLockTypeE type, BOOL nonblock)
{
	BOOL skip = FALSE;
	BOOL done = FALSE;
	BCRESULT result = BC_R_SUCCESS;

	ASSERT(VALID_RWLOCK(this));

	m_sLock.Lock();

	if (type == bc_rwlocktype_read)
	{
		if (this->m_nReadersWaiting != 0)
			skip = TRUE;
		while (!done)
		{
			if (!skip &&
			    ((this->m_nActive == 0 ||
			      (this->m_eType == bc_rwlocktype_read &&
			       (this->m_nWritersWaiting == 0 ||
				this->m_nGranted < this->m_nReadQuota)))))
			{
				this->m_eType = bc_rwlocktype_read;
				this->m_nActive++;
				this->m_nGranted++;
				done = TRUE;
			}
			else if (nonblock)
			{
				result = BC_R_LOCKBUSY;
				done = TRUE;
			}
			else
			{
				skip = FALSE;
				this->m_nReadersWaiting++;
				this->m_sCondReadable.Wait();
				this->m_nReadersWaiting--;
			}
		}
	}
	else
	{
		if (this->m_nWritersWaiting != 0)
			skip = TRUE;
		while (!done)
		{
			if (!skip && this->m_nActive == 0)
			{
				this->m_eType = bc_rwlocktype_write;
				this->m_nActive = 1;
				this->m_nGranted++;
				done = TRUE;
			}
			else if (nonblock)
			{
				result = BC_R_LOCKBUSY;
				done = TRUE;
			}
			else
			{
				skip = FALSE;
				this->m_nWritersWaiting++;
				this->m_sCondWriteable.Wait();
				this->m_nWritersWaiting--;
			}
		}
	}

	m_sLock.Unlock();

	return (result);
}

BCRESULT
BCRWLock::Lock(BCRWLockTypeE type)
{
	return (_doit(type, FALSE));
}

BCRESULT
BCRWLock::TryLock(BCRWLockTypeE type)
{
	return (_doit(type, TRUE));
}

BCRESULT
BCRWLock::TryUpgrade()
{
	BCRESULT result = BC_R_SUCCESS;

	ASSERT(VALID_RWLOCK(this));
	m_sLock.Lock();
	ASSERT(this->m_eType == bc_rwlocktype_read);
	ASSERT(this->m_nActive != 0);

	/* If we are the only reader then succeed. */
	if (this->m_nActive == 1)
	{
		this->m_eOriginal = (this->m_eOriginal == bc_rwlocktype_none) ?
				bc_rwlocktype_read : bc_rwlocktype_none;
		this->m_eType = bc_rwlocktype_write;
	} else
		result = BC_R_LOCKBUSY;

	m_sLock.Unlock();
	return (result);
}

void
BCRWLock::Downgrade()
{
	ASSERT(VALID_RWLOCK(this));
	m_sLock.Lock();
	ASSERT(this->m_eType == bc_rwlocktype_write);
	ASSERT(this->m_nActive == 1);

	this->m_eType = bc_rwlocktype_read;
	this->m_eOriginal = (this->m_eOriginal == bc_rwlocktype_none) ?
			bc_rwlocktype_write : bc_rwlocktype_none;
	/*
	 * Resume processing any read request that were blocked when
	 * we upgraded.
	 */
	if (this->m_eOriginal == bc_rwlocktype_none &&
	    (this->m_nWritersWaiting == 0 || this->m_nGranted < this->m_nReadQuota) &&
	    this->m_nReadersWaiting > 0)
		this->m_sCondReadable.Broadcast();

	m_sLock.Unlock();
}

BCRESULT
BCRWLock::Unlock(BCRWLockTypeE type)
{
	ASSERT(VALID_RWLOCK(this));
	m_sLock.Lock();
	ASSERT(this->m_eType == type);

	UNUSED(type);

	ASSERT(this->m_nActive > 0);
	this->m_nActive--;
	if (this->m_nActive == 0)
	{
		if (this->m_eOriginal != bc_rwlocktype_none)
		{
			this->m_eType = this->m_eOriginal;
			this->m_eOriginal = bc_rwlocktype_none;
		}
		if (this->m_eType == bc_rwlocktype_read)
		{
			this->m_nGranted = 0;
			if (this->m_nWritersWaiting > 0)
			{
				this->m_eType = bc_rwlocktype_write;
				this->m_sCondWriteable.Signal();
			}
			else if (this->m_nReadersWaiting > 0)
			{
				/* Does this case ever happen? */
				this->m_sCondReadable.Broadcast();
			}
		}
		else
		{
			if (this->m_nReadersWaiting > 0)
			{
				if (this->m_nWritersWaiting > 0 &&
				    this->m_nGranted < this->m_nWriteQuota)
				{
					this->m_sCondWriteable.Signal();
				}
				else
				{
					this->m_nGranted = 0;
					this->m_eType = bc_rwlocktype_read;
					this->m_sCondReadable.Broadcast();
				}
			}
			else if (this->m_nWritersWaiting > 0)
			{
				this->m_nGranted = 0;
				this->m_sCondWriteable.Signal();
			}
			else
			{
				this->m_nGranted = 0;
			}
		}
	}
	ASSERT(this->m_eOriginal == bc_rwlocktype_none);

	m_sLock.Unlock();

	return (BC_R_SUCCESS);
}

#endif /* BC_PLATFORM_HAVEXADD && BC_PLATFORM_HAVECMPXCHG */
#else /* BC_PLATFORM_USETHREADS */

BCRESULT
BCRWLock::Initialize(unsigned int read_quota, unsigned int write_quota)
{
	ASSERT(this != NULL);

	UNUSED(read_quota);
	UNUSED(write_quota);

	this->m_eType = bc_rwlocktype_read;
	this->m_nActive = 0;
	this->m_nMagic = RWLOCK_MAGIC;

	return (BC_R_SUCCESS);
}

BCRESULT
BCRWLock::Lock(BCRWLockTypeE type)
{
	REQUIRE(VALID_RWLOCK(this));

	if (type == bc_rwlocktype_read)
	{
		if (this->m_eType != bc_rwlocktype_read && this->m_nActive != 0)
			return (BC_R_LOCKBUSY);
		this->m_eType = bc_rwlocktype_read;
		this->m_nActive++;
	}
	else
	{
		if (this->m_nActive != 0)
			return (BC_R_LOCKBUSY);
		this->m_eType = bc_rwlocktype_write;
		this->m_nActive = 1;
	}
	return (BC_R_SUCCESS);
}

BCRESULT
BCRWLock::TryLock(BCRWLockTypeE type)
{
	return (Lock(type));
}

BCRESULT
BCRWLock::TryUpgrade()
{
	BCRESULT result = BC_R_SUCCESS;

	ASSERT(VALID_RWLOCK(this));
	ASSERT(this->m_eType == bc_rwlocktype_read);
	ASSERT(this->m_nActive != 0);

	/* If we are the only reader then succeed. */
	if (this->m_nActive == 1)
		this->m_eType = bc_rwlocktype_write;
	else
		result = BC_R_LOCKBUSY;
	return (result);
}

void
BCRWLock::Downgrade()
{
	ASSERT(VALID_RWLOCK(this));
	ASSERT(this->m_eType == bc_rwlocktype_write);
	ASSERT(this->m_nActive == 1);

	this->m_eType = bc_rwlocktype_read;
}

BCRESULT
BCRWLock::Unlock(BCRWLockTypeE type)
{
	ASSERT(VALID_RWLOCK(this));
	ASSERT(this->m_eType == type);

	UNUSED(type);

	ASSERT(this->m_nActive > 0);
	this->m_nActive--;

	return (BC_R_SUCCESS);
}

void
BCRWLock::Destroy()
{
	REQUIRE(this != NULL);
	REQUIRE(this->m_nActive == 0);
	this->m_nMagic = 0;
}

#endif /* BC_PLATFORM_USETHREADS */

///////////////////////////////////////////////////////////////////////////////
// End of namespace : BC
///////////////////////////////////////////////////////////////////////////////

}

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
