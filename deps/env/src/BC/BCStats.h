///////////////////////////////////////////////////////////////////////////////
// file : BCStats.h
// author : antoniozhou
///////////////////////////////////////////////////////////////////////////////


#ifndef BC_BCSTATS_H_INCLUDED__
#define BC_BCSTATS_H_INCLUDED__

#include <BC/Exports.h>
#include <BC/BCMagic.h>
#include <BC/BCRWLock.h>
#include <BC/BCMemPool.h>

///////////////////////////////////////////////////////////////////////////////
// Namespace : BC
///////////////////////////////////////////////////////////////////////////////

namespace BC
{

/*%<
* Flag(s) for BCStats::Dump().
*/
#define BC_STATSDUMP_VERBOSE	0x00000001 /*%< dump 0-value counters */

typedef int					BCStatsCounterType;

/*%<
* Dump callback type.
*/
typedef void (*BCStatsDumperPtr)(BCStatsCounterType, uint64_t, void *);
typedef BCStatsDumperPtr		LPFN_BCStatsDumper;



#ifndef BC_STATS_USEMULTIFIELDS
#if defined(BC_RWLOCK_USEATOMIC) && defined(BC_PLATFORM_HAVEXADD) && !defined(BC_PLATFORM_HAVEXADDQ)
#define BC_STATS_USEMULTIFIELDS 1
#else
#define BC_STATS_USEMULTIFIELDS 0
#endif
#endif	/* BC_STATS_USEMULTIFIELDS */

#if BC_STATS_USEMULTIFIELDS
typedef struct {
	bc_atomic_t hi;
	bc_atomic_t lo;
} BCStatType;
#else
typedef uint64_t BCStatType;
#endif

///////////////////////////////////////////////////////////////////////////////
// class : BCStats
///////////////////////////////////////////////////////////////////////////////

class BCStats : public BCMagic
{
	DECLARE_FIXED_ALLOC(BCStats);

public:
	BCStats();
	~BCStats();

	BCRESULT			Create(int ncounters);
	/*%<
	* Create a statistics counter structure of general type.  It counts a general
	* set of counters indexed by an ID between 0 and ncounters -1.
	*
	* Requires:
	*
	* Returns:
	*\li	BC_R_SUCCESS	-- all ok
	*
	*\li	anything else	-- failure
	*/

	void				Attach(BCStats **statsp);
	/*%<
	* Attach to a statistics set.
	*
	* Requires:
	*\li	'stats' is a valid BCStats.
	*
	*\li	'statsp' != NULL && '*statsp' == NULL
	*/

	void				Detach(BCStats **statsp);
	/*%<
	* Detaches from the statistics set.
	*
	* Requires:
	*\li	'statsp' != NULL and '*statsp' is a valid BCStats.
	*/

	int					GetNumOfCounters();
	/*%<
	* Returns the number of counters contained in stats.
	*
	*/

	void				Increment(BCStatsCounterType counter);
	/*%<
	* Increment the counter-th counter of stats.
	*
	* Requires:
	*\li	counter is less than the maximum available ID for the stats specified
	*	on creation.
	*/

	void				Decrement(BCStatsCounterType counter);
	/*%<
	* Decrement the counter-th counter of stats.
	*
	* Requires:
	*\li	'stats' is a valid BCStats.
	*/

	void				Dump(
							LPFN_BCStatsDumper dump_fn,
							void *arg,
							unsigned int options);
	/*%<
	* Dump the current statistics counters in a specified way.  For each counter
	* in stats, dump_fn is called with its current value and the given argument
	* arg.  By default counters that have a value of 0 is skipped; if options has
	* the BC_STATSDUMP_VERBOSE flag, even such counters are dumped.
	*
	*/

protected:
	void				_CopyCounters();
private:
	DECLARE_NO_COPY_CLASS(BCStats);
	StackMemPool			m_sPool;
	int						m_nCounters;

	BCSpinMutex				m_sLock;
	unsigned int			m_nRef; /* locked by lock */

	/*%
	 * Locked by counterlock or unlocked if efficient rwlock is not
	 * available.
	 */
#ifdef BC_RWLOCK_USEATOMIC
	BCRWLock				m_sCounterRWLock;
#endif
	BCStatType			*	m_pCounters;

	/*%
	 * We don't want to lock the counters while we are dumping, so we first
	 * copy the current counter values into a local array.  This buffer
	 * will be used as the copy destination.  It's allocated on creation
	 * of the stats structure so that the dump operation won't fail due
	 * to memory allocation failure.
	 * XXX: this approach is weird for non-threaded build because the
	 * additional memory and the copy overhead could be avoided.  We prefer
	 * simplicity here, however, under the assumption that this function
	 * should be only rarely called.
	 */
	uint64_t			*	m_pCopiedCounters;
};

///////////////////////////////////////////////////////////////////////////////
// End of namespace : BC
///////////////////////////////////////////////////////////////////////////////

} // End of namespace BC

#endif // BC_BCSTATS_H_INCLUDED__

///////////////////////////////////////////////////////////////////////////////
// End of file : BCStats.h
///////////////////////////////////////////////////////////////////////////////
