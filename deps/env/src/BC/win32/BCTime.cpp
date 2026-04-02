
#include <sys/timeb.h>
#include <time.h>
#include <stdint.h>
#include <BC/Utils.h>
#include "BC/BCThread.h"
#include "BC/BCTime.h"

///////////////////////////////////////////////////////////////////////////////
// Namespace: BC
///////////////////////////////////////////////////////////////////////////////

namespace BC
{

///////////////////////////////////////////////////////////////////////////////
// time Utilities
///////////////////////////////////////////////////////////////////////////////

#define IsLeapYear(y) ((!(y % 4)) ? (((!(y % 400)) && (y % 100)) ? 1 : 0) : 0)

static DWORD get_local_timezone(TIME_ZONE_INFORMATION **tzresult)
{
    static TIME_ZONE_INFORMATION tz;
    static DWORD result;
    static int init = 0;

    if (!init)
	{
        result = GetTimeZoneInformation(&tz);
        init = 1;
    }

    *tzresult = &tz;
    return result;
}

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
	LONGLONG nwtime = 0;
	FILETIME time;
#ifndef _WIN32_WCE
	GetSystemTimeAsFileTime(&time);
#else
	SYSTEMTIME st;
	GetSystemTime(&st);
	SystemTimeToFileTime(&st, &time);
#endif
	FileTimeToBCTime(&nwtime, &time);
	return nwtime;
}

BCRESULT bc_time_exp_gmt(BCTimeExpS *result, bc_time_t input)
{
    FILETIME ft;
    SYSTEMTIME st;
    BCTimeToFileTime(&ft, input);
    FileTimeToSystemTime(&ft, &st);
    /* The Platform SDK documents that SYSTEMTIME/FILETIME are
     * generally UTC, so no timezone info needed
     */
    SystemTimeToBCExpTime(result, &st);
    result->tm_usec = (int32_t) (input % BC_USEC_PER_SEC);
    return BC_R_SUCCESS;
}

BCRESULT bc_time_exp_tz(BCTimeExpS *result, bc_time_t input, int32_t offs)
{
    FILETIME ft;
    SYSTEMTIME st;
    BCTimeToFileTime(&ft, input + (offs *  BC_USEC_PER_SEC));
    FileTimeToSystemTime(&ft, &st);
    /* The Platform SDK documents that SYSTEMTIME/FILETIME are
     * generally UTC, so we will simply note the offs used.
     */
    SystemTimeToBCExpTime(result, &st);
    result->tm_usec = (int32_t) (input % BC_USEC_PER_SEC);
    result->tm_gmtoff = offs;
    return BC_R_SUCCESS;
}

BCRESULT bc_time_exp_lt(BCTimeExpS *result, bc_time_t input)
{
    SYSTEMTIME st;
    FILETIME ft, localft;
    TIME_ZONE_INFORMATION *tz;
    SYSTEMTIME localst;
	bc_time_t localtime;

	BCTimeToFileTime(&ft, input);

    get_local_timezone(&tz);

    FileTimeToSystemTime(&ft, &st);

    /* The Platform SDK documents that SYSTEMTIME/FILETIME are
     * generally UTC.  We use SystemTimeToTzSpecificLocalTime
     * because FileTimeToLocalFileFime is documented that the
     * resulting time local file time would have DST relative
     * to the *present* date, not the date converted.
     */
    SystemTimeToTzSpecificLocalTime(tz, &st, &localst);
    SystemTimeToBCExpTime(result, &localst);
    result->tm_usec = (int32_t) (input % BC_USEC_PER_SEC);


    /* Recover the resulting time as an nw time and use the
     * delta for gmtoff in seconds (and ignore msec rounding)
     */
    SystemTimeToFileTime(&localst, &localft);
    FileTimeToBCTime(&localtime, &localft);
    result->tm_gmtoff = (int)bc_time_sec(localtime)
                        - (int)bc_time_sec(input);

    /* To compute the dst flag, we compare the expected
     * local (standard) timezone bias to the delta.
     * [Note, in war time or double daylight time the
     * resulting tm_isdst is, desireably, 2 hours]
     */
    result->tm_isdst = (result->tm_gmtoff / 3600)
                        - (-(tz->Bias + tz->StandardBias) / 60);

    return BC_R_SUCCESS;
}

BCRESULT bc_time_exp_get(bc_time_t *t, BCTimeExpS *xt)
{
    bc_time_t year = xt->tm_year;
    bc_time_t days;
    static const int dayoffset[12] =
    {306, 337, 0, 31, 61, 92, 122, 153, 184, 214, 245, 275};

    /* shift new year to 1st March in order to make leap year calc easy */

    if (xt->tm_mon < 2)
        year--;

    /* Find number of days since 1st March 1900 (in the Gregorian calendar). */

    days = year * 365 + year / 4 - year / 100 + (year / 100 + 3) / 4;
    days += dayoffset[xt->tm_mon] + xt->tm_mday - 1;
    days -= 25508;              /* 1 jan 1970 is 25508 days since 1 mar 1900 */

    days = ((days * 24 + xt->tm_hour) * 60 + xt->tm_min) * 60 + xt->tm_sec;

    if (days < 0)
	{
        return BC_R_FAILURE;
    }
    *t = days * BC_USEC_PER_SEC + xt->tm_usec;
    return BC_R_SUCCESS;
}

BCRESULT bc_time_exp_gmt_get(bc_time_t *t, BCTimeExpS *xt)
{
    BCRESULT status = bc_time_exp_get(t, xt);
    if (status == BC_R_SUCCESS)
        *t -= (bc_time_t) xt->tm_gmtoff * BC_USEC_PER_SEC;
    return status;
}

// Win32 does not provide gettimeofday, so we emulate it to simplify the
// Timer code.

#define DELTA_EPOCH_IN_TICKS  116444736000000000ULL

int
gettimeofday(struct timeval* tv, struct timezone* tz)
{
	FILETIME    ft;
	uint64_t    tmpres;
	static int  tzflag;
	if (NULL != tv) {
		GetSystemTimeAsFileTime(&ft);
		tmpres = ((uint64_t)ft.dwHighDateTime << 32) | (ft.dwLowDateTime);
		tv->tv_sec = (long)((tmpres - DELTA_EPOCH_IN_TICKS) / 10000000ULL);
		tv->tv_usec = (long)((tmpres % 10000000ULL) / 10ULL);
	}

	if (NULL != tz) {
		if (!tzflag) {
			_tzset();
			tzflag++;
		}
		tz->tz_minuteswest = _timezone / 60;
		tz->tz_dsttime = _daylight;
	}

	return 0;
}

void gettimeofday_from_1970(struct timeval* tv, void*)
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

uint32_t GetTimeMillis()
{
	timeval tv;
	gettimeofday_from_1970(&tv, NULL);
	return (tv.tv_sec * 1000 + tv.tv_usec/1000);
}

void bc_stdtime_get(uint32_t *pTime)
{
	/*
	 * Set 't' to the number of seconds past 00:00:00 UTC, January 1, 1970.
	 */

	ASSERT(pTime != NULL);

	time_t now;
	(void)time(&now);
	*pTime = now;
}

///////////////////////////////////////////////////////////////////////////////
// time
///////////////////////////////////////////////////////////////////////////////


/*
 * struct FILETIME uses "100-nanoseconds intervals".
 * NS / S = 1000000000 (10^9).
 * While it is reasonably obvious that this makes the needed
 * conversion factor 10^7, it is coded this way for additional clarity.
 */
#define NS_PER_S 	1000000000
#define NS_INTERVAL	100
#define INTERVALS_PER_S (NS_PER_S / NS_INTERVAL)

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
	TCHAR DateBuf[50];
	TCHAR TimeBuf[50];

	static const char badtime[] = "99-Bad-9999 99:99:99.999";

	ASSERT(len > 0);
	if (FileTimeToLocalFileTime(&t->absolute, &localft) &&
	    FileTimeToSystemTime(&localft, &st))
	{
		GetDateFormat(LOCALE_USER_DEFAULT, 0, &st, TEXT("dd-MMM-yyyy"),
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
	TCHAR DateBuf[50];
	TCHAR TimeBuf[50];

	static const char badtime[] = "99-Bad-9999 99:99:99.999";

	ASSERT(len > 0);
	if (FileTimeToLocalFileTime(&t->absolute, &localft) &&
		FileTimeToSystemTime(&localft, &st))
	{
		GetDateFormat(LOCALE_USER_DEFAULT, 0, &st, TEXT("dd-MM-yyyy"),
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
	TCHAR DateBuf[50];
	TCHAR TimeBuf[50];

/* strftime() format: "%a, %d %b %Y %H:%M:%S GMT" */

	ASSERT(len > 0);
	if (FileTimeToSystemTime(&t->absolute, &st))
	{
		GetDateFormat(LOCALE_USER_DEFAULT, 0, &st,
			      TEXT("ddd',' dd-MMM-yyyy"), DateBuf, 50);
		GetTimeFormat(LOCALE_USER_DEFAULT,
			      TIME_NOTIMEMARKER | TIME_FORCE24HOURFORMAT,
			      &st, TEXT("hh':'mm':'ss"), TimeBuf, 50);

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
	TCHAR DateBuf[50];
	TCHAR TimeBuf[50];

/* strftime() format: "%a, %d %b %Y %H:%M:%S GMT" */

	ASSERT(len > 0);
	if (FileTimeToSystemTime(&t->absolute, &st))
	{
		GetDateFormat(LOCALE_USER_DEFAULT, 0, &st,
			      TEXT("ddd',' dd-MMM-yyyy"), DateBuf, 50);
		GetTimeFormat(LOCALE_USER_DEFAULT,
			      TIME_NOTIMEMARKER | TIME_FORCE24HOURFORMAT,
			      &st, TEXT("hh':'mm':'ss"), TimeBuf, 50);

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
	TCHAR DateBuf[50];
	TCHAR TimeBuf[50];

/* strtime() format: "%Y-%m-%dT%H:%M:%SZ" */

	ASSERT(len > 0);
	if (FileTimeToSystemTime(&t->absolute, &st))
	{
		GetDateFormat(LOCALE_NEUTRAL, 0, &st, TEXT("yyyy-MM-dd"),
			      DateBuf, 50);
		GetTimeFormat(LOCALE_NEUTRAL,
			      TIME_NOTIMEMARKER | TIME_FORCE24HOURFORMAT,
			      &st, TEXT("hh':'mm':'ss"), TimeBuf, 50);
		snprintf(buf, len, "%s%sZ", DateBuf, TimeBuf);
	}
	else
	{
		buf[0] = 0;
	}
}

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
	if (microseconds > ((uint64_t)0xFFFFFFFF) * 1000)
	{
		seconds = 0xFFFFFFFF;
		nanoseconds  = 0;
	}
	else
	{
		seconds = (DWORD)(microseconds / 1000000);
		nanoseconds = (DWORD)((microseconds%1000000)*1000);
	}

	nRetVal = pCond->Wait(seconds, nanoseconds);
	return (nRetVal == 0?BC_R_TIMEDOUT:BC_R_SUCCESS);
}

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

char *
bc_time_formathttptime(const BCTimeS *t, char *buf, unsigned int len)
{
	bc_time_t t1;

	FileTimeToBCTime(&t1, &t->absolute);
	return bc_time_formathttptime(t1, buf, len);
}

char *
bc_time_formathttpcookietime(bc_time_t t, char *buf, unsigned int len)
{
    BCTimeExpS  tm;

	//bc_time_exp_gmt(&tm, t);
	bc_gmtime(t, &tm);

    /*
     * Netscape 3.x does not understand 4-digit years at all and
     * 2-digit years more than "37"
     */

    snprintf(buf, len,
            (tm.tm_year > 2037) ?
                "%s, %02d-%s-%d %02d:%02d:%02d GMT":
                "%s, %02d-%s-%02d %02d:%02d:%02d GMT",
            week[tm.tm_wday],
            tm.tm_mday,
            months[tm.tm_mon - 1],
            (tm.tm_year > 2037) ? tm.tm_year: tm.tm_year % 100,
            tm.tm_hour,
            tm.tm_min,
            tm.tm_sec);
	return buf;
}

char *
bc_time_formathttpcookietime(const BCTimeS *t, char *buf, unsigned int len)
{
	bc_time_t t1;

	FileTimeToBCTime(&t1, &t->absolute);
	return bc_time_formathttpcookietime(t1, buf, len);
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
     * see src/http/bc_http_parse_time.c
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