
#ifndef BC_BCRWLOCK_H_INCLUDED__
#define BC_BCRWLOCK_H_INCLUDED__

#include <BC/Exports.h>
#include <BC/BCThread.h>
#include <BC/BCMagic.h>

///////////////////////////////////////////////////////////////////////////////
// Namespace : BC
///////////////////////////////////////////////////////////////////////////////

namespace BC
{

typedef enum BCRWLockTypeE
{
	bc_rwlocktype_none	= 0,
	bc_rwlocktype_read	= 1,
	bc_rwlocktype_write = 2
}BCRWLockTypeE;

#ifdef BC_PLATFORM_USETHREADS
#if defined(BC_PLATFORM_HAVEXADD) && defined(BC_PLATFORM_HAVECMPXCHG)
#define BC_RWLOCK_USEATOMIC 1
#endif // BC_PLATFORM_HAVEXADD && BC_PLATFORM_HAVECMPXCHG
#endif // BC_PLATFORM_USETHREADS

///////////////////////////////////////////////////////////////////////////////
// class : BCRWLock
///////////////////////////////////////////////////////////////////////////////

class BCRWLock : public BCMagic
{
public:
	class BASE_EXPORT Owner
	{
	public:
		Owner(BCRWLock& m, BCRWLockTypeE type) : m_mutex(m), m_type(type)
		{
			m_mutex.Lock(m_type);
		}
		~Owner(void)
		{
			m_mutex.Unlock(m_type);
		}
	private:
		// dummy copy constructor and operator= to prevent copying
		DECLARE_NO_COPY_CLASS(Owner);
		BCRWLock		&	m_mutex;
		BCRWLockTypeE		m_type;
	};
public:
	BCRWLock();
	~BCRWLock();

	BCRESULT			Create(
							unsigned int read_quota,
							unsigned int write_quota);
	BCRESULT			Lock(BCRWLockTypeE type);

	BCRESULT			TryLock(BCRWLockTypeE type);

	BCRESULT			Unlock(BCRWLockTypeE type);

	BCRESULT			TryUpgrade();

	void				Downgrade();

	void				Destroy();
protected:
#if defined(BC_PLATFORM_HAVEXADD) && defined(BC_PLATFORM_HAVECMPXCHG)
#else
	BCRESULT			_doit(BCRWLockTypeE type, BOOL nonblock);
#endif
private:
	DECLARE_NO_COPY_CLASS(BCRWLock);

#ifdef BC_PLATFORM_USETHREADS

	BCMutex				m_sLock;

#if defined(BC_PLATFORM_HAVEXADD) && defined(BC_PLATFORM_HAVECMPXCHG)
	/*
	 * When some atomic instructions with hardware assistance are
	 * available, rwlock will use those so that concurrent readers do not
	 * interfere with each other through mutex as long as no writers
	 * appear, massively reducing the lock overhead in the typical case.
	 *
	 * The basic algorithm of this approach is the "simple
	 * writer-preference lock" shown in the following URL:
	 * http://www.cs.rochester.edu/u/scott/synchronization/pseudocode/rw.html
	 * but our implementation does not rely on the spin lock unlike the
	 * original algorithm to be more portable as a user space application.
	 */

	/* Read or modified atomically. */
	bc_atomic_t			m_nWriteRequests;
	bc_atomic_t			m_nWriteCompletions;
	bc_atomic_t			m_nCntAndFlag;

	/* Locked by lock. */
	BCCondition			m_sCondReadable;
	BCCondition			m_sCondWriteable;
	unsigned int		m_nReadersWaiting;

	/* Locked by rwlock itself. */
	unsigned int		m_nWriteGranted;

	/* Unlocked. */
	unsigned int		m_nWriteQuota;

#else  /* BC_PLATFORM_HAVEXADD && BC_PLATFORM_HAVECMPXCHG */

	/*%< Locked by lock. */
	BCCondition			m_sCondReadable;
	BCCondition			m_sCondWriteable;
	BCRWLockTypeE		m_eType;

	/*% The number of threads that have the lock. */
	unsigned int		m_nActive;

	/*%
	 * The number of lock grants made since the lock was last switched
	 * from reading to writing or vice versa; used in determining
	 * when the quota is reached and it is time to switch.
	 */
	unsigned int		m_nGranted;

	unsigned int		m_nReadersWaiting;
	unsigned int		m_nWritersWaiting;
	unsigned int		m_nReadQuota;
	unsigned int		m_nWriteQuota;
	BCRWLockTypeE		m_eOriginal;
#endif  /* BC_PLATFORM_HAVEXADD && BC_PLATFORM_HAVECMPXCHG */
#else /* BC_PLATFORM_USETHREADS */
	BCRWLockTypeE		m_eType;
	unsigned int		m_nActive;
#endif /* BC_PLATFORM_USETHREADS */
};

///////////////////////////////////////////////////////////////////////////////
// End of namespace : BC
///////////////////////////////////////////////////////////////////////////////

}

#endif

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
