
#ifdef _WIN32
#include <sys/timeb.h>
#include <time.h>
#else // !_WIN32
#include <BC/Utils.h>
#endif // _WIN32
#include <BC/BCThread.h>
#include <BC/BCTime.h>
// #include <BC/BCLogger.h>

///////////////////////////////////////////////////////////////////////////////
// Namespace: BC
///////////////////////////////////////////////////////////////////////////////

namespace BC
{

///////////////////////////////////////////////////////////////////////////////
// time Utilities
///////////////////////////////////////////////////////////////////////////////

#define IsLeapYear(y) ((!(y % 4)) ? (((!(y % 400)) && (y % 100)) ? 1 : 0) : 0)

#ifdef _WIN32

static void SystemTimeToBCExpTime(BCTimeExpS *xt, SYSTEMTIME *tm)
{
	static const int dayoffset[12] =
	{0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};

	/* Note; the caller is responsible for filling in detailed tm_usec,
	* tm_gmtoff and tm_isdst data when applicable.
	*/
	xt->tm_usec = tm->wMilliseconds * 1000;
	xt->tm_sec  = tm->wSecond;
	xt->tm_min  = tm->wMinute;
	xt->tm_hour = tm->wHour;
	xt->tm_mday = tm->wDay;
	xt->tm_mon  = tm->wMonth - 1;
	xt->tm_year = tm->wYear - 1900;
	xt->tm_wday = tm->wDayOfWeek;
	xt->tm_yday = dayoffset[xt->tm_mon] + (tm->wDay - 1);
	xt->tm_isdst = 0;
	xt->tm_gmtoff = 0;

	/* If this is a leap year, and we're past the 28th of Feb. (the
	* 58th day after Jan. 1), we'll increment our tm_yday by one.
	*/
	if (IsLeapYear(tm->wYear) && (xt->tm_yday > 58))
		xt->tm_yday++;
}

/* Return micro-seconds since the Unix epoch (jan. 1, 1970) UTC */
bc_time_t bc_time_now(void)
{
	LONGLONG aprtime = 0;
	FILETIME time;
#ifndef _WIN32_WCE
	GetSystemTimeAsFileTime(&time);
#else
	SYSTEMTIME st;
	GetSystemTime(&st);
	SystemTimeToFileTime(&st, &time);
#endif
	FileTimeToBCTime(&aprtime, &time);
	return aprtime;
}

// Win32 does not provide gettimeofday, so we emulate it to simplify the
// Timer code.
void gettimeofday(struct timeval* tv, void*)
{
	LARGE_INTEGER counts, countsPerSec;
	static double usecPerCount = 0.0;

	if (QueryPerformanceCounter(&counts))
	{
		if (usecPerCount == 0.0)
		{
			QueryPerformanceFrequency(&countsPerSec);
			usecPerCount = 1000000.0 / countsPerSec.QuadPart;
		}

		LONGLONG usecs = (LONGLONG)(counts.QuadPart * usecPerCount);
		tv->tv_usec = (long)(usecs % 1000000);
		tv->tv_sec = (long)(usecs / 1000000);

	}
	else
	{
		struct timeb tb;
		ftime(&tb);
		tv->tv_sec = (long)tb.time;
		tv->tv_usec = tb.millitm * 1000;
	}
}
#else // !_WIN32
bc_time_t bc_time_now()
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return tv.tv_sec * BC_USEC_PER_SEC + tv.tv_usec;
}
#endif // _WIN32


uint32_t GetTimeMillis()
{
	timeval tv;
	gettimeofday(&tv, NULL);
	return (tv.tv_sec * 1000 + tv.tv_usec/1000);
}

///////////////////////////////////////////////////////////////////////////////
// time
///////////////////////////////////////////////////////////////////////////////

#ifdef _WIN32

/*
 * struct FILETIME uses "100-nanoseconds intervals".
 * NS / S = 1000000000 (10^9).
 * While it is reasonably obvious that this makes the needed
 * conversion factor 10^7, it is coded this way for additional clarity.
 */
#define NS_PER_S 	1000000000
#define NS_INTERVAL	100
#define INTERVALS_PER_S (NS_PER_S / NS_INTERVAL)
#define UINT64_MAX	_UI64_MAX

/***
 *** Absolute Times
 ***/

static BCTimeS epoch = { { 0, 0 } };
BC_API BCTimeS *bc_time_epoch = &epoch;

/***
 *** Intervals
 ***/

static BCIntervalS zero_interval = { 0 };
BC_API BCIntervalS *bc_interval_zero = &zero_interval;

void
bc_interval_set(BCIntervalS *i, unsigned int seconds,
		 unsigned int nanoseconds)
{
	ASSERT(i != NULL);
	ASSERT(nanoseconds < NS_PER_S);

	/*
	 * This rounds nanoseconds up not down.
	 */
	i->interval = (LONGLONG)seconds * INTERVALS_PER_S
		+ (nanoseconds + NS_INTERVAL - 1) / NS_INTERVAL;
}

BOOL
bc_interval_iszero(const BCIntervalS *i)
{
	ASSERT(i != NULL);
	if (i->interval == 0)
		return (TRUE);

	return (FALSE);
}

void
bc_time_set(BCTimeS *t, unsigned int seconds, unsigned int nanoseconds)
{
	SYSTEMTIME epoch = { 1970, 1, 4, 1, 0, 0, 0, 0 };
	FILETIME temp;
	ULARGE_INTEGER i1;

	ASSERT(t != NULL);
	ASSERT(nanoseconds < NS_PER_S);

	SystemTimeToFileTime(&epoch, &temp);

	i1.LowPart = t->absolute.dwLowDateTime;
	i1.HighPart = t->absolute.dwHighDateTime;

	i1.QuadPart += (unsigned __int64)nanoseconds/100;
	i1.QuadPart += (unsigned __int64)seconds*10000000;

	t->absolute.dwLowDateTime = i1.LowPart;
	t->absolute.dwHighDateTime = i1.HighPart;
}

void
bc_time_settoepoch(BCTimeS *t)
{
	ASSERT(t != NULL);

	t->absolute.dwLowDateTime = 0;
	t->absolute.dwHighDateTime = 0;
}

BOOL
bc_time_isepoch(const BCTimeS *t)
{
	ASSERT(t != NULL);

	if (t->absolute.dwLowDateTime == 0 &&
	    t->absolute.dwHighDateTime == 0)
		return (TRUE);

	return (FALSE);
}

BCRESULT
bc_time_now(BCTimeS *t)
{
	ASSERT(t != NULL);

	GetSystemTimeAsFileTime(&t->absolute);

	return (BC_R_SUCCESS);
}

BCRESULT
bc_time_nowplusinterval(BCTimeS *t, const BCIntervalS *i)
{
	ULARGE_INTEGER i1;

	ASSERT(t != NULL);
	ASSERT(i != NULL);

	GetSystemTimeAsFileTime(&t->absolute);

	i1.LowPart = t->absolute.dwLowDateTime;
	i1.HighPart = t->absolute.dwHighDateTime;

	if (UINT64_MAX - i1.QuadPart < (unsigned __int64)i->interval)
		return (BC_R_RANGE);

	i1.QuadPart += i->interval;

	t->absolute.dwLowDateTime  = i1.LowPart;
	t->absolute.dwHighDateTime = i1.HighPart;

	return (BC_R_SUCCESS);
}

int
bc_time_compare(const BCTimeS *t1, const BCTimeS *t2)
{
	ASSERT(t1 != NULL && t2 != NULL);

	return ((int)CompareFileTime(&t1->absolute, &t2->absolute));
}

BCRESULT
bc_time_add(const BCTimeS *t, const BCIntervalS *i, BCTimeS *result)
{
	ULARGE_INTEGER i1;

	ASSERT(t != NULL && i != NULL && result != NULL);

	i1.LowPart = t->absolute.dwLowDateTime;
	i1.HighPart = t->absolute.dwHighDateTime;

	if (UINT64_MAX - i1.QuadPart < (unsigned __int64)i->interval)
		return (BC_R_RANGE);

	i1.QuadPart += i->interval;

	result->absolute.dwLowDateTime = i1.LowPart;
	result->absolute.dwHighDateTime = i1.HighPart;

	return (BC_R_SUCCESS);
}

BCRESULT
bc_time_subtract(const BCTimeS *t, const BCIntervalS *i,
		  BCTimeS *result)
{
	ULARGE_INTEGER i1;

	ASSERT(t != NULL && i != NULL && result != NULL);

	i1.LowPart = t->absolute.dwLowDateTime;
	i1.HighPart = t->absolute.dwHighDateTime;

	if (i1.QuadPart < (unsigned __int64) i->interval)
		return (BC_R_RANGE);

	i1.QuadPart -= i->interval;

	result->absolute.dwLowDateTime = i1.LowPart;
	result->absolute.dwHighDateTime = i1.HighPart;

	return (BC_R_SUCCESS);
}

uint64_t
bc_time_microdiff(const BCTimeS *t1, const BCTimeS *t2)
{
	ULARGE_INTEGER i1, i2;
	LONGLONG i3;

	ASSERT(t1 != NULL && t2 != NULL);

	i1.LowPart  = t1->absolute.dwLowDateTime;
	i1.HighPart = t1->absolute.dwHighDateTime;
	i2.LowPart  = t2->absolute.dwLowDateTime;
	i2.HighPart = t2->absolute.dwHighDateTime;

	if (i1.QuadPart <= i2.QuadPart)
		return (0);

	/*
	 * Convert to microseconds.
	 */
	i3 = (i1.QuadPart - i2.QuadPart) / 10;

	return (i3);
}

uint32_t
bc_time_seconds(const BCTimeS *t)
{
	SYSTEMTIME epoch = { 1970, 1, 4, 1, 0, 0, 0, 0 };
	FILETIME temp;
	ULARGE_INTEGER i1, i2;
	LONGLONG i3;

	SystemTimeToFileTime(&epoch, &temp);

	i1.LowPart  = t->absolute.dwLowDateTime;
	i1.HighPart = t->absolute.dwHighDateTime;
	i2.LowPart  = temp.dwLowDateTime;
	i2.HighPart = temp.dwHighDateTime;

	i3 = (i1.QuadPart - i2.QuadPart) / 10000000;

	return ((uint32_t)i3);
}

uint32_t
bc_time_nanoseconds(const BCTimeS *t)
{
	ULARGE_INTEGER i;

	i.LowPart  = t->absolute.dwLowDateTime;
	i.HighPart = t->absolute.dwHighDateTime;
	return ((uint32_t)(i.QuadPart % 10000000) * 100);
}

void
bc_time_formattimestamp(const BCTimeS *t, char *buf, unsigned int len)
{
	FILETIME localft;
	SYSTEMTIME st;
	char DateBuf[50];
	char TimeBuf[50];

	static const char badtime[] = "99-Bad-9999 99:99:99.999";

	ASSERT(len > 0);
	if (FileTimeToLocalFileTime(&t->absolute, &localft) &&
	    FileTimeToSystemTime(&localft, &st))
	{
		GetDateFormat(LOCALE_USER_DEFAULT, 0, &st, "dd-MMM-yyyy",
			      DateBuf, 50);
		GetTimeFormat(LOCALE_USER_DEFAULT, TIME_NOTIMEMARKER|
			      TIME_FORCE24HOURFORMAT, &st, NULL, TimeBuf, 50);

		snprintf(buf, len, "%s %s.%03u", DateBuf, TimeBuf,
			 st.wMilliseconds);
	}
	else
	{
		snprintf(buf, len, badtime);
	}
}

void
bc_time_formattimestamp2(const BCTimeS *t, char *buf, unsigned int len)
{
	FILETIME localft;
	SYSTEMTIME st;
	char DateBuf[50];
	char TimeBuf[50];

	static const char badtime[] = "99-Bad-9999 99:99:99.999";

	ASSERT(len > 0);
	if (FileTimeToLocalFileTime(&t->absolute, &localft) &&
		FileTimeToSystemTime(&localft, &st))
	{
		GetDateFormat(LOCALE_USER_DEFAULT, 0, &st, "dd-MM-yyyy",
			DateBuf, 50);
		GetTimeFormat(LOCALE_USER_DEFAULT, TIME_NOTIMEMARKER|
			TIME_FORCE24HOURFORMAT, &st, NULL, TimeBuf, 50);

		snprintf(buf, len, "%s %s.%03u", DateBuf, TimeBuf,
			st.wMilliseconds);
	}
	else
	{
		snprintf(buf, len, badtime);
	}
}

void
bc_time_formathttptimestamp(const BCTimeS *t, char *buf, unsigned int len)
{
	SYSTEMTIME st;
	char DateBuf[50];
	char TimeBuf[50];

/* strftime() format: "%a, %d %b %Y %H:%M:%S GMT" */

	ASSERT(len > 0);
	if (FileTimeToSystemTime(&t->absolute, &st))
	{
		GetDateFormat(LOCALE_USER_DEFAULT, 0, &st,
			      "ddd',', dd-MMM-yyyy", DateBuf, 50);
		GetTimeFormat(LOCALE_USER_DEFAULT,
			      TIME_NOTIMEMARKER | TIME_FORCE24HOURFORMAT,
			      &st, "hh':'mm':'ss", TimeBuf, 50);

		snprintf(buf, len, "%s %s GMT", DateBuf, TimeBuf);
	}
	else
	{
		buf[0] = 0;
	}
}

void
bc_time_formathttptimestamp2(const BCTimeS *t, char *buf, unsigned int len)
{
	SYSTEMTIME st;
	char DateBuf[50];
	char TimeBuf[50];

/* strftime() format: "%a, %d %b %Y %H:%M:%S GMT" */

	ASSERT(len > 0);
	if (FileTimeToSystemTime(&t->absolute, &st))
	{
		GetDateFormat(LOCALE_USER_DEFAULT, 0, &st,
			      "dd',dd-MM-yyyy", DateBuf, 50);
		GetTimeFormat(LOCALE_USER_DEFAULT,
			      TIME_NOTIMEMARKER | TIME_FORCE24HOURFORMAT,
			      &st, "hh':'mm':'ss", TimeBuf, 50);

		snprintf(buf, len, "%s %s GMT", DateBuf, TimeBuf);
	}
	else
	{
		buf[0] = 0;
	}
}

void
bc_time_formatISO8601(const BCTimeS *t, char *buf, unsigned int len)
{
	SYSTEMTIME st;
	char DateBuf[50];
	char TimeBuf[50];

/* strtime() format: "%Y-%m-%dT%H:%M:%SZ" */

	ASSERT(len > 0);
	if (FileTimeToSystemTime(&t->absolute, &st))
	{
		GetDateFormat(LOCALE_NEUTRAL, 0, &st, "yyyy-MM-dd",
			      DateBuf, 50);
		GetTimeFormat(LOCALE_NEUTRAL,
			      TIME_NOTIMEMARKER | TIME_FORCE24HOURFORMAT,
			      &st, "hh':'mm':'ss", TimeBuf, 50);
		snprintf(buf, len, "%s%sZ", DateBuf, TimeBuf);
	}
	else
	{
		buf[0] = 0;
	}
}

void bc_stdtime_get(uint32_t *pTime)
{
	/*
	 * Set 't' to the number of seconds past 00:00:00 UTC, January 1, 1970.
	 */

	ASSERT(pTime != NULL);

	(void)time((time_t *)pTime);
}

#else // !_WIN32

#define NS_PER_S	1000000000	/*%< Nanoseconds per second. */
#define NS_PER_US	1000		/*%< Nanoseconds per microsecond. */
#define US_PER_S	1000000		/*%< Microseconds per second. */

/*
 * All of the INSIST()s checks of nanoseconds < NS_PER_S are for
 * consistency checking of the type. In lieu of magic numbers, it
 * is the best we've got.  The check is only performed on functions which
 * need an initialized type.
 */

#ifndef BC_FIX_TV_USEC
#define BC_FIX_TV_USEC 1
#endif

/*%
 *** Intervals
 ***/

static BCIntervalS zero_interval = { 0, 0 };
BCIntervalS *bc_interval_zero = &zero_interval;

#if BC_FIX_TV_USEC
static inline void
fix_tv_usec(struct timeval *tv)
{
	BOOL fixed = FALSE;

	if (tv->tv_usec < 0)
	{
		fixed = TRUE;
		do
		{
			tv->tv_sec -= 1;
			tv->tv_usec += US_PER_S;
		} while (tv->tv_usec < 0);
	}
	else if (tv->tv_usec >= US_PER_S)
	{
		fixed = TRUE;
		do
		{
			tv->tv_sec += 1;
			tv->tv_usec -= US_PER_S;
		} while (tv->tv_usec >=US_PER_S);
	}
	/*
	 * Call LogError directly as was are called from the logging functions.
	 */
	// if (fixed)
	// 	(void)LogError(_LOCAL_, "gettimeofday returned bad tv_usec: corrected");
}
#endif

void
bc_interval_set(BCIntervalS *i,
		 unsigned int seconds, unsigned int nanoseconds)
{
	REQUIRE(i != NULL);
	REQUIRE(nanoseconds < NS_PER_S);

	i->seconds = seconds;
	i->nanoseconds = nanoseconds;
}

BOOL
bc_interval_iszero(const BCIntervalS *i)
{
	REQUIRE(i != NULL);
	INSIST(i->nanoseconds < NS_PER_S);

	if (i->seconds == 0 && i->nanoseconds == 0)
		return (TRUE);

	return (FALSE);
}

/***
 *** Absolute Times
 ***/

static BCTimeS epoch = { 0, 0 };
BCTimeS *bc_time_epoch = &epoch;

void
bc_time_set(BCTimeS *t, unsigned int seconds, unsigned int nanoseconds)
{
	REQUIRE(t != NULL);
	REQUIRE(nanoseconds < NS_PER_S);

	t->seconds = seconds;
	t->nanoseconds = nanoseconds;
}

void
bc_time_settoepoch(BCTimeS *t)
{
	REQUIRE(t != NULL);

	t->seconds = 0;
	t->nanoseconds = 0;
}

BOOL
bc_time_isepoch(const BCTimeS *t)
{
	REQUIRE(t != NULL);
	INSIST(t->nanoseconds < NS_PER_S);

	if (t->seconds == 0 && t->nanoseconds == 0)
		return (TRUE);

	return (FALSE);
}


BCRESULT
bc_time_now(BCTimeS *t)
{
	struct timeval tv;
	char strbuf[BC_STRERRORSIZE];

	REQUIRE(t != NULL);

	if (gettimeofday(&tv, NULL) == -1)
	{
		// bc_strerror(errno, strbuf, sizeof(strbuf));
		// LogError(_LOCAL_, "%s", strbuf);
		return (BC_R_UNEXPECTED);
	}

	/*
	 * Does POSIX guarantee the signedness of tv_sec and tv_usec?  If not,
	 * then this test will generate warnings for platforms on which it is
	 * unsigned.  In any event, the chances of any of these problems
	 * happening are pretty much zero, but since the libisc library ensures
	 * certain things to be true ...
	 */
#if BC_FIX_TV_USEC
	fix_tv_usec(&tv);
	if (tv.tv_sec < 0)
		return (BC_R_UNEXPECTED);
#else
	if (tv.tv_sec < 0 || tv.tv_usec < 0 || tv.tv_usec >= US_PER_S)
		return (BC_R_UNEXPECTED);
#endif

	/*
	 * Ensure the tv_sec value fits in t->seconds.
	 */
	if (sizeof(tv.tv_sec) > sizeof(t->seconds) &&
	    ((tv.tv_sec | (unsigned int)-1) ^ (unsigned int)-1) != 0U)
		return (BC_R_RANGE);

	t->seconds = tv.tv_sec;
	t->nanoseconds = tv.tv_usec * NS_PER_US;

	return (BC_R_SUCCESS);
}

BCRESULT
bc_time_nowplusinterval(BCTimeS *t, const BCIntervalS *i)
{
	struct timeval tv;
	char strbuf[BC_STRERRORSIZE];

	REQUIRE(t != NULL);
	REQUIRE(i != NULL);
	INSIST(i->nanoseconds < NS_PER_S);

	if (gettimeofday(&tv, NULL) == -1)
	{
		// bc_strerror(errno, strbuf, sizeof(strbuf));
		// LogError(_LOCAL_, "%s", strbuf);
		return (BC_R_UNEXPECTED);
	}

	/*
	 * Does POSIX guarantee the signedness of tv_sec and tv_usec?  If not,
	 * then this test will generate warnings for platforms on which it is
	 * unsigned.  In any event, the chances of any of these problems
	 * happening are pretty much zero, but since the libisc library ensures
	 * certain things to be true ...
	 */
#if BC_FIX_TV_USEC
	fix_tv_usec(&tv);
	if (tv.tv_sec < 0)
		return (BC_R_UNEXPECTED);
#else
	if (tv.tv_sec < 0 || tv.tv_usec < 0 || tv.tv_usec >= US_PER_S)
		return (BC_R_UNEXPECTED);
#endif

	/*
	 * Ensure the resulting seconds value fits in the size of an
	 * unsigned int.  (It is written this way as a slight optimization;
	 * note that even if both values == INT_MAX, then when added
	 * and getting another 1 added below the result is UINT_MAX.)
	 */
	if ((tv.tv_sec > INT_MAX || i->seconds > INT_MAX) &&
	    ((long long)tv.tv_sec + i->seconds > UINT_MAX))
		return (BC_R_RANGE);

	t->seconds = tv.tv_sec + i->seconds;
	t->nanoseconds = tv.tv_usec * NS_PER_US + i->nanoseconds;
	if (t->nanoseconds >= NS_PER_S)
	{
		t->seconds++;
		t->nanoseconds -= NS_PER_S;
	}

	return (BC_R_SUCCESS);
}

int
bc_time_compare(const BCTimeS *t1, const BCTimeS *t2)
{
	REQUIRE(t1 != NULL && t2 != NULL);
	INSIST(t1->nanoseconds < NS_PER_S && t2->nanoseconds < NS_PER_S);

	if (t1->seconds < t2->seconds)
		return (-1);
	if (t1->seconds > t2->seconds)
		return (1);
	if (t1->nanoseconds < t2->nanoseconds)
		return (-1);
	if (t1->nanoseconds > t2->nanoseconds)
		return (1);
	return (0);
}

BCRESULT
bc_time_add(const BCTimeS *t, const BCIntervalS *i, BCTimeS *result)
{
	REQUIRE(t != NULL && i != NULL && result != NULL);
	INSIST(t->nanoseconds < NS_PER_S && i->nanoseconds < NS_PER_S);

	/*
	 * Ensure the resulting seconds value fits in the size of an
	 * unsigned int.  (It is written this way as a slight optimization;
	 * note that even if both values == INT_MAX, then when added
	 * and getting another 1 added below the result is UINT_MAX.)
	 */
	if ((t->seconds > INT_MAX || i->seconds > INT_MAX) &&
	    ((long long)t->seconds + i->seconds > UINT_MAX))
		return (BC_R_RANGE);

	result->seconds = t->seconds + i->seconds;
	result->nanoseconds = t->nanoseconds + i->nanoseconds;
	if (result->nanoseconds >= NS_PER_S)
	{
		result->seconds++;
		result->nanoseconds -= NS_PER_S;
	}

	return (BC_R_SUCCESS);
}

BCRESULT
bc_time_subtract(const BCTimeS *t, const BCIntervalS *i,
		  BCTimeS *result)
{
	REQUIRE(t != NULL && i != NULL && result != NULL);
	INSIST(t->nanoseconds < NS_PER_S && i->nanoseconds < NS_PER_S);

	if ((unsigned int)t->seconds < i->seconds ||
	    ((unsigned int)t->seconds == i->seconds &&
	     t->nanoseconds < i->nanoseconds))
	    return (BC_R_RANGE);

	result->seconds = t->seconds - i->seconds;
	if (t->nanoseconds >= i->nanoseconds)
	{
		result->nanoseconds = t->nanoseconds - i->nanoseconds;
	}
	else
	{
		result->nanoseconds = NS_PER_S - i->nanoseconds + t->nanoseconds;
		result->seconds--;
	}

	return (BC_R_SUCCESS);
}

uint64_t
bc_time_microdiff(const BCTimeS *t1, const BCTimeS *t2)
{
	uint64_t i1, i2, i3;

	REQUIRE(t1 != NULL && t2 != NULL);
	INSIST(t1->nanoseconds < NS_PER_S && t2->nanoseconds < NS_PER_S);

	i1 = (uint64_t)t1->seconds * NS_PER_S + t1->nanoseconds;
	i2 = (uint64_t)t2->seconds * NS_PER_S + t2->nanoseconds;

	if (i1 <= i2)
		return (0);

	i3 = i1 - i2;

	/*
	 * Convert to microseconds.
	 */
	i3 = (i1 - i2) / NS_PER_US;

	return (i3);
}

uint32_t
bc_time_seconds(const BCTimeS *t)
{
	REQUIRE(t != NULL);
	INSIST(t->nanoseconds < NS_PER_S);

	return ((uint32_t)t->seconds);
}

BCRESULT
bc_time_secondsastimet(const BCTimeS *t, time_t *secondsp)
{
	uint64_t i;
	time_t seconds;

	REQUIRE(t != NULL);
	INSIST(t->nanoseconds < NS_PER_S);

	/*
	 * Ensure that the number of seconds represented by t->seconds
	 * can be represented by a time_t.  Since t->seconds is an unsigned
	 * int and since time_t is mostly opaque, this is trickier than
	 * it seems.  (This standardized opaqueness of time_t is *very*
	 * frustrating; time_t is not even limited to being an integral
	 * type.)
	 *
	 * The mission, then, is to avoid generating any kind of warning
	 * about "signed versus unsigned" while trying to determine if the
	 * the unsigned int t->seconds is out range for tv_sec, which is
	 * pretty much only true if time_t is a signed integer of the same
	 * size as the return value of bc_time_seconds.
	 *
	 * The use of the 64 bit integer ``i'' takes advantage of C's
	 * conversion rules to either zero fill or sign extend the widened
	 * type.
	 *
	 * Solaris 5.6 gives this warning about the left shift:
	 *	warning: integer overflow detected: op "<<"
	 * if the U(nsigned) qualifier is not on the 1.
	 */
	seconds = (time_t)t->seconds;

	INSIST(sizeof(unsigned int) == sizeof(uint32_t));
	INSIST(sizeof(time_t) >= sizeof(uint32_t));

	if (sizeof(time_t) == sizeof(uint32_t) &&	       /* Same size. */
	    (time_t)0.5 != 0.5 &&	       /* Not a floating point type. */
	    (i = (time_t)-1) != 4294967295u &&		       /* Is signed. */
	    (seconds &
	     (1U << (sizeof(time_t) * CHAR_BIT - 1))) != 0U)
	     {   /* Negative. */
		/*
		 * This UNUSED() is here to shut up the IRIX compiler:
		 *	variable "i" was set but never used
		 * when the value of i *was* used in the third test.
		 * (Let's hope the compiler got the actual test right.)
		 */
		UNUSED(i);
		return (BC_R_RANGE);
	}

	*secondsp = seconds;

	return (BC_R_SUCCESS);
}

uint32_t
bc_time_nanoseconds(const BCTimeS *t)
{
	REQUIRE(t != NULL);

	ENSURE(t->nanoseconds < NS_PER_S);

	return ((uint32_t)t->nanoseconds);
}

void
bc_time_formattimestamp(const BCTimeS *t, char *buf, unsigned int len)
{
	time_t now;
	unsigned int flen;

	REQUIRE(len > 0);

	now = (time_t) t->seconds;
	flen = strftime(buf, len, "%d-%b-%Y %X", localtime(&now));
	INSIST(flen < len);
	if (flen != 0)
		snprintf(buf + flen, len - flen,
			 ".%03u", t->nanoseconds / 1000000);
	else
		snprintf(buf, len, "99-Bad-9999 99:99:99.999");
}

void
bc_time_formathttptimestamp(const BCTimeS *t, char *buf, unsigned int len)
{
	time_t now;
	unsigned int flen;

	REQUIRE(len > 0);

	now = (time_t)t->seconds;
	flen = strftime(buf, len, "%a, %d %b %Y %H:%M:%S GMT", gmtime(&now));
	INSIST(flen < len);
}

void
bc_time_formatISO8601(const BCTimeS *t, char *buf, unsigned int len)
{
	time_t now;
	unsigned int flen;

	REQUIRE(len > 0);

	now = (time_t)t->seconds;
	flen = strftime(buf, len, "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));
	INSIST(flen < len);
}

///////////////////////////////////////////////////////////////////////////////
// std time
///////////////////////////////////////////////////////////////////////////////

void
bc_stdtime_get(uint32_t *t)
{
	struct timeval tv;

	/*
	 * Set 't' to the number of seconds since 00:00:00 UTC, January 1,
	 * 1970.
	 */

	REQUIRE(t != NULL);

	RUNTIME_CHECK(gettimeofday(&tv, NULL) != -1);

#if BC_FIX_TV_USEC
	fix_tv_usec(&tv);
	INSIST(tv.tv_usec >= 0);
#else
	INSIST(tv.tv_usec >= 0 && tv.tv_usec < US_PER_S);
#endif

	*t = (unsigned int)tv.tv_sec;
}

#endif // _WIN32

#ifdef _WIN32
BCRESULT
bc_condition_waituntil(BCCondition *pCond,	BCTimeS *pTime)
{
	DWORD seconds, nanoseconds;
	uint64_t microseconds;
	BCTimeS now;
	int32_t nRetVal;

	if (bc_time_now(&now) != BC_R_SUCCESS)
	{
		/* XXX */
		return (BC_R_UNEXPECTED);
	}

	microseconds = bc_time_microdiff(pTime, &now);
	if (microseconds > 0xFFFFFFFFi64 * 1000000)
	{
		seconds = 0xFFFFFFFF;
		nanoseconds  = 0;
	}
	else
	{
		seconds = (DWORD)(microseconds / 1000000);
		nanoseconds = (DWORD)((microseconds%1000000)*1000000);
	}

	nRetVal = pCond->Wait(seconds, nanoseconds);
	return (nRetVal == 0?BC_R_TIMEDOUT:BC_R_SUCCESS);
}
#else // !_WIN32

BCRESULT
bc_condition_waituntil(BCCondition *pCond,	BCTimeS *pTime)
{
	return pCond->TimedWait(pTime);
}

#endif // _WIN32

///////////////////////////////////////////////////////////////////////////////
// time utilities : borrow from nginx
///////////////////////////////////////////////////////////////////////////////

void
bc_gmtime(bc_time_t t, BCTimeExpS *tp);

static const char  *week[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char  *months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                           "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

char *
bc_time_formathttptime(bc_time_t t, char *buf, unsigned int len)
{
    BCTimeExpS  tm;

	//bc_time_exp_gmt(&tm, t);
	bc_gmtime(t, &tm);

    snprintf(buf, len, "%s, %02d %s %4d %02d:%02d:%02d GMT",
                week[tm.tm_wday],
                tm.tm_mday,
                months[tm.tm_mon - 1],
                tm.tm_year,
                tm.tm_hour,
                tm.tm_min,
                tm.tm_sec);
	return buf;
}

void
bc_gmtime(bc_time_t t, BCTimeExpS *tp)
{
    int64_t   yday;
    uint64_t  n, sec, min, hour, mday, mon, year, wday, days, leap;

    /* the calculation is valid for positive time_t only */

    n = (uint64_t) t;

    days = n / 86400;

    /* Jaunary 1, 1970 was Thursday */

    wday = (4 + days) % 7;

    n %= 86400;
    hour = n / 3600;
    n %= 3600;
    min = n / 60;
    sec = n % 60;

    /*
     * the algorithm based on Gauss' formula,
     */

    /* days since March 1, 1 BC */
    days = days - (31 + 28) + 719527;

    /*
     * The "days" should be adjusted to 1 only, however, some March 1st's go
     * to previous year, so we adjust them to 2.  This causes also shift of the
     * last Feburary days to next year, but we catch the case when "yday"
     * becomes negative.
     */

    year = (days + 2) * 400 / (365 * 400 + 100 - 4 + 1);

    yday = days - (365 * year + year / 4 - year / 100 + year / 400);

    if (yday < 0) 
	{
        leap = (year % 4 == 0) && (year % 100 || (year % 400 == 0));
        yday = 365 + leap + yday;
        year--;
    }

    /*
     * The empirical formula that maps "yday" to month.
     * There are at least 10 variants, some of them are:
     *     mon = (yday + 31) * 15 / 459
     *     mon = (yday + 31) * 17 / 520
     *     mon = (yday + 31) * 20 / 612
     */

    mon = (yday + 31) * 10 / 306;

    /* the Gauss' formula that evaluates days before the month */

    mday = yday - (367 * mon / 12 - 30) + 1;

    if (yday >= 306) 
	{
        year++;
        mon -= 10;

        /*
         * there is no "yday" in Win32 SYSTEMTIME
         *
         * yday -= 306;
         */
    }
	else
	{
        mon += 2;

        /*
         * there is no "yday" in Win32 SYSTEMTIME
         *
         * yday += 31 + 28 + leap;
         */
    }

    tp->tm_sec = (int32_t) sec;
    tp->tm_min = (int32_t) min;
    tp->tm_hour = (int32_t) hour;
    tp->tm_mday = (int32_t) mday;
    tp->tm_mon = (int32_t) mon;
    tp->tm_year = (int32_t) year;
    tp->tm_wday = (int32_t) wday;
	tp->tm_yday = (int32_t)yday;
}

///////////////////////////////////////////////////////////////////////////////
// Namespace: BC
///////////////////////////////////////////////////////////////////////////////

} // End of namespace BC


///////////////////////////////////////////////////////////////////////////////
// End of file...
///////////////////////////////////////////////////////////////////////////////
