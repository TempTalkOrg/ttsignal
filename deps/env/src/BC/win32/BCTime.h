
#ifndef BC_WIN32_TIME_H_INCLUDED__
#define BC_WIN32_TIME_H_INCLUDED__

#include <BC/Exports.h>

///////////////////////////////////////////////////////////////////////////////
// Namespace: BC
///////////////////////////////////////////////////////////////////////////////

namespace BC
{

#ifndef __GNUC__
#define BC_INT64_C(val) (val##i64)
#define BC_UINT64_C(val) (val##Ui64)
#else
#define BC_INT64_C(val) (val##LL)
#define BC_UINT64_C(val) (val##ULL)
#endif

#define BC_TIME_C(val) BC_INT64_C(val)

#define BC_INT64_T_FMT          "I64d"
#define BC_UINT64_T_FMT         "I64u"
#define BC_UINT64_T_HEX_FMT     "I64x"

///////////////////////////////////////////////////////////////////////////////
// time utilities
///////////////////////////////////////////////////////////////////////////////

/** number of microseconds since 00:00:00 january 1, 1970 UTC */
typedef int64_t bc_time_t;

/** mechanism to properly type bc_time_t literals */
#define BC_TIME_C(val) BC_INT64_C(val)

/** mechanism to properly print bc_time_t values */
#define BC_TIME_T_FMT BC_INT64_T_FMT

/** intervals for I/O timeouts, in microseconds */
typedef int64_t bc_interval_time_t;
/** short interval for I/O timeouts, in microseconds */
typedef int32_t bc_short_interval_time_t;

/** number of microseconds per second */
#define BC_USEC_PER_SEC BC_TIME_C(1000000)

/** @return bc_time_t as a second */
#define bc_time_sec(time) ((time) / BC_USEC_PER_SEC)

/** @return bc_time_t as a usec */
#define bc_time_usec(time) ((time) % BC_USEC_PER_SEC)

/** @return bc_time_t as a msec */
#define bc_time_msec(time) (((time) / 1000) % 1000)

/** @return bc_time_t as a msec */
#define bc_time_as_msec(time) ((time) / 1000)

/** @return a second as an bc_time_t */
#define bc_time_from_sec(sec) ((bc_time_t)(sec) * BC_USEC_PER_SEC)

/** @return a second and usec combination as an bc_time_t */
#define bc_time_make(sec, usec) ((bc_time_t)(sec) * BC_USEC_PER_SEC \
+ (bc_time_t)(usec))


/**
* @return the current time
*/
BC_API bc_time_t bc_time_now(void);

struct timezone {
	int tz_minuteswest;     /* minutes west of Greenwich */
	int tz_dsttime;         /* type of DST correction */
};

BC_API int gettimeofday(struct timeval* t,struct timezone* timezone);

BC_API uint32_t GetTimeMillis();

BC_API void bc_stdtime_get(uint32_t *pTime);

/** @see bc_time_exp_t */
typedef struct BCTimeExpS BCTimeExpS;

/**
* a structure similar to ANSI struct tm with the following differences:
*  - tm_usec isn't an ANSI field
*  - tm_gmtoff isn't an ANSI field (it's a bsdism)
*/
struct BCTimeExpS {
	/** microseconds past tm_sec */
	int32_t tm_usec;
	/** (0-61) seconds past tm_min */
	int32_t tm_sec;
	/** (0-59) minutes past tm_hour */
	int32_t tm_min;
	/** (0-23) hours past midnight */
	int32_t tm_hour;
	/** (1-31) day of the month */
	int32_t tm_mday;
	/** (0-11) month of the year */
	int32_t tm_mon;
	/** year since 1900 */
	int32_t tm_year;
	/** (0-6) days since sunday */
	int32_t tm_wday;
	/** (0-365) days since jan 1 */
	int32_t tm_yday;
	/** daylight saving time */
	int32_t tm_isdst;
	/** seconds east of UTC */
	int32_t tm_gmtoff;
};

/**
 * convert a time to its human readable components using an offset
 * from GMT
 * @param result the exploded time
 * @param input the time to explode
 * @param offs the number of seconds offset to apply
 */
BC_API
BCRESULT bc_time_exp_tz(BCTimeExpS *result,
                        bc_time_t input,
                        int32_t offs);

/**
 * convert a time to its human readable components in GMT timezone
 * @param result the exploded time
 * @param input the time to explode
 */
BC_API
BCRESULT bc_time_exp_gmt(BCTimeExpS *result,
                         bc_time_t input);

/**
 * convert a time to its human readable components in local timezone
 * @param result the exploded time
 * @param input the time to explode
 */
BC_API
BCRESULT bc_time_exp_lt(BCTimeExpS *result,
                        bc_time_t input);

/**
 * Convert time value from human readable format to a numeric bc_time_t
 * e.g. elapsed usec since epoch
 * @param result the resulting imploded time
 * @param input the input exploded time
 */
BC_API
BCRESULT bc_time_exp_get(bc_time_t *result,
                         BCTimeExpS *input);

/**
 * Convert time value from human readable format to a numeric bc_time_t that
 * always represents GMT
 * @param result the resulting imploded time
 * @param input the input exploded time
 */
BC_API
BCRESULT bc_time_exp_gmt_get(bc_time_t *result,
                             BCTimeExpS *input);

/* Number of micro-seconds between the beginning of the Windows epoch
* (Jan. 1, 1601) and the Unix epoch (Jan. 1, 1970)
*/
#define BC_DELTA_EPOCH_IN_USEC   BC_TIME_C(11644473600000000);


static inline void FileTimeToBCTime(bc_time_t *result, const FILETIME *pft)
{
	/* Convert FILETIME one 64 bit number so we can work with it. */
	*result = pft->dwHighDateTime;
	*result = (*result) << 32;
	*result |= pft->dwLowDateTime;
	*result /= 10;    /* Convert from 100 nano-sec periods to micro-seconds. */
	*result -= BC_DELTA_EPOCH_IN_USEC;  /* Convert from Windows epoch to Unix epoch */
	return;
}


static inline void BCTimeToFileTime(LPFILETIME pft, bc_time_t t)
{
	LONGLONG ll;
	t += BC_DELTA_EPOCH_IN_USEC;
	ll = t * 10;
	pft->dwLowDateTime = (DWORD)ll;
	pft->dwHighDateTime = (DWORD) (ll >> 32);
	return;
}


///////////////////////////////////////////////////////////////////////////////
// time
///////////////////////////////////////////////////////////////////////////////


/***
 *** Intervals
 ***/

/*
 * The contents of this structure are private, and MUST NOT be accessed
 * directly by callers.
 *
 * The contents are exposed only to allow callers to avoid dynamic allocation.
 */
typedef struct BCIntervalS {
	int64_t interval;
}BCIntervalS;

BC_API extern BCIntervalS *bc_interval_zero;

BC_API
void
bc_interval_set(BCIntervalS *i,
		 unsigned int seconds, unsigned int nanoseconds);
/*
 * Set 'i' to a value representing an interval of 'seconds' seconds and
 * 'nanoseconds' nanoseconds, suitable for use in bc_time_add() and
 * bc_time_subtract().
 *
 * Requires:
 *
 *	't' is a valid pointer.
 *	nanoseconds < 1000000000.
 */

BC_API
BOOL
bc_interval_iszero(const BCIntervalS *i);
/*
 * Returns BC_TRUE iff. 'i' is the zero interval.
 *
 * Requires:
 *
 *	'i' is a valid pointer.
 */

/***
 *** Absolute Times
 ***/

/*
 * The contents of this structure are private, and MUST NOT be accessed
 * directly by callers.
 *
 * The contents are exposed only to allow callers to avoid dynamic allocation.
 */

typedef struct BCTimeS
{
	FILETIME absolute;
}BCTimeS;

BC_API extern BCTimeS *bc_time_epoch;

BC_API
void
bc_time_set(BCTimeS *t, unsigned int seconds, unsigned int nanoseconds);
/*%<
 * Set 't' to a value which represents the given number of seconds and
 * nanoseconds since 00:00:00 January 1, 1970, UTC.
 *
 * Requires:
 *\li   't' is a valid pointer.
 *\li   nanoseconds < 1000000000.
 */

BC_API
void
bc_time_settoepoch(BCTimeS *t);
/*
 * Set 't' to the time of the epoch.
 *
 * Notes:
 * 	The date of the epoch is platform-dependent.
 *
 * Requires:
 *
 *	't' is a valid pointer.
 */

BC_API
BOOL
bc_time_isepoch(const BCTimeS *t);
/*
 * Returns BC_TRUE iff. 't' is the epoch ("time zero").
 *
 * Requires:
 *
 *	't' is a valid pointer.
 */

BC_API
BCRESULT
bc_time_now(BCTimeS *t);
/*
 * Set 't' to the current absolute time.
 *
 * Requires:
 *
 *	't' is a valid pointer.
 *
 * Returns:
 *
 *	Success
 *	Unexpected error
 *		Getting the time from the system failed.
 *	Out of range
 *		The time from the system is too large to be represented
 *		in the current definition of BCTimeS.
 */

BC_API
BCRESULT
bc_time_nowplusinterval(BCTimeS *t, const BCIntervalS *i);
/*
 * Set *t to the current absolute time + i.
 *
 * Note:
 *	This call is equivalent to:
 *
 *		bc_time_now(t);
 *		bc_time_add(t, i, t);
 *
 * Requires:
 *
 *	't' and 'i' are valid pointers.
 *
 * Returns:
 *
 *	Success
 *	Unexpected error
 *		Getting the time from the system failed.
 *	Out of range
 *		The interval added to the time from the system is too large to
 *		be represented in the current definition of BCTimeS.
 */

BC_API
int
bc_time_compare(const BCTimeS *t1, const BCTimeS *t2);
/*
 * Compare the times referenced by 't1' and 't2'
 *
 * Requires:
 *
 *	't1' and 't2' are valid pointers.
 *
 * Returns:
 *
 *	-1		t1 < t2		(comparing times, not pointers)
 *	0		t1 = t2
 *	1		t1 > t2
 */

BC_API
BCRESULT
bc_time_add(const BCTimeS *t, const BCIntervalS *i, BCTimeS *result);
/*
 * Add 'i' to 't', storing the result in 'result'.
 *
 * Requires:
 *
 *	't', 'i', and 'result' are valid pointers.
 *
 * Returns:
 * 	Success
 *	Out of range
 * 		The interval added to the time is too large to
 *		be represented in the current definition of BCTimeS.
 */

BC_API
BCRESULT
bc_time_subtract(const BCTimeS *t, const BCIntervalS *i,
		  BCTimeS *result);
/*
 * Subtract 'i' from 't', storing the result in 'result'.
 *
 * Requires:
 *
 *	't', 'i', and 'result' are valid pointers.
 *
 * Returns:
 *	Success
 *	Out of range
 *		The interval is larger than the time since the epoch.
 */

BC_API
uint64_t
bc_time_microdiff(const BCTimeS *t1, const BCTimeS *t2);
/*
 * Find the difference in milliseconds between time t1 and time t2.
 * t2 is the subtrahend of t1; ie, difference = t1 - t2.
 *
 * Requires:
 *
 *	't1' and 't2' are valid pointers.
 *
 * Returns:
 *	The difference of t1 - t2, or 0 if t1 <= t2.
 */

BC_API
uint32_t
bc_time_nanoseconds(const BCTimeS *t);
/*
 * Return the number of nanoseconds stored in a time structure.
 *
 * Notes:
 *	This is the number of nanoseconds in excess of the number
 *	of seconds since the epoch; it will always be less than one
 *	full second.
 *
 * Requires:
 *	't' is a valid pointer.
 *
 * Ensures:
 *	The returned value is less than 1*10^9.
 */

BC_API
void
bc_time_formattimestamp(const BCTimeS *t, char *buf, unsigned int len);
/*
 * Format the time 't' into the buffer 'buf' of length 'len',
 * using a format like "30-Aug-2000 04:06:47.997" and the local time zone.
 * If the text does not fit in the buffer, the result is indeterminate,
 * but is always guaranteed to be null terminated.
 *
 *  Requires:
 *      'len' > 0
 *      'buf' points to an array of at least len chars
 *
 */

BC_API
void
bc_time_formattimestamp2(const BCTimeS *t, char *buf, unsigned int len);
/*
 * Format the time 't' into the buffer 'buf' of length 'len',
 * using a format like "30-Aug-2000 04:06:47.997" and the local time zone.
 * If the text does not fit in the buffer, the result is indeterminate,
 * but is always guaranteed to be null terminated.
 *
 *  Requires:
 *      'len' > 0
 *      'buf' points to an array of at least len chars
 *
 */

BC_API
void
bc_time_formathttptimestamp(const BCTimeS *t, char *buf, unsigned int len);
/*
 * Format the time 't' into the buffer 'buf' of length 'len',
 * using a format like "Mon, 30 Aug 2000 04:06:47 GMT"
 * If the text does not fit in the buffer, the result is indeterminate,
 * but is always guaranteed to be null terminated.
 *
 *  Requires:
 *      'len' > 0
 *      'buf' points to an array of at least len chars
 *
 */

BC_API
void
bc_time_formathttptimestamp2(const BCTimeS *t, char *buf, unsigned int len);
/*
 * Format the time 't' into the buffer 'buf' of length 'len',
 * using a format like "Mon, 30 Aug 2000 04:06:47 GMT"
 * If the text does not fit in the buffer, the result is indeterminate,
 * but is always guaranteed to be null terminated.
 *
 *  Requires:
 *      'len' > 0
 *      'buf' points to an array of at least len chars
 *
 */

BC_API
void
bc_time_formatISO8601(const BCTimeS *t, char *buf, unsigned int len);
/*%<
 * Format the time 't' into the buffer 'buf' of length 'len',
 * using the ISO8601 format: "yyyy-mm-ddThh:mm:ssZ"
 * If the text does not fit in the buffer, the result is indeterminate,
 * but is always guaranteed to be null terminated.
 *
 *  Requires:
 *\li      'len' > 0
 *\li      'buf' points to an array of at least len chars
 *
 */

BC_API
uint32_t
bc_time_seconds(const BCTimeS *t);

class BCCondition;

BC_API
BCRESULT
bc_condition_waituntil(BCCondition *pCond,	BCTimeS *pTime);

///////////////////////////////////////////////////////////////////////////////
// http time format utilities :
///////////////////////////////////////////////////////////////////////////////

BC_API
char *
bc_time_formathttptime(bc_time_t t, char *buf, unsigned int len);

BC_API
char *
bc_time_formathttptime(const BCTimeS *t, char *buf, unsigned int len);

BC_API
char *
bc_time_formathttpcookietime(bc_time_t t, char *buf, unsigned int len);

BC_API
char *
bc_time_formathttpcookietime(const BCTimeS *t, char *buf, unsigned int len);

///////////////////////////////////////////////////////////////////////////////
// Namespace: BC
///////////////////////////////////////////////////////////////////////////////

} // End of namespace BC


#endif // BC_WIN32_TIME_H_INCLUDED__

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////