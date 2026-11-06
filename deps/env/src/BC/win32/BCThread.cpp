
#include <BC/Utils.h>
#include <process.h>
#include <BC/BCException.h>
#include <BC/BCThread.h>

#pragma warning(disable:4297)	//

///////////////////////////////////////////////////////////////////////////////
// Namespace: BC
///////////////////////////////////////////////////////////////////////////////

namespace BC
{


#ifdef MUTEX_USE_CRITICAL_SECTION
#define SleepConditionVariable		SleepConditionVariableCS
#define CV_SLEEP_FLAGS				
#else // MUTEX_USE_CRITICAL_SECTION
#define SleepConditionVariable		SleepConditionVariableSRW
#define CV_SLEEP_FLAGS				, 0
#endif // MUTEX_USE_CRITICAL_SECTION

#define DB(x) // x
//#include <iostream.h> or #include <iostream> if DB is on.

static void get_time_now(unsigned long* abs_sec, unsigned long* abs_nsec);

struct THREADNAME_INFO
{
	DWORD dwType;     // must be 0x1000
	LPCSTR szName;    // pointer to name (in user addr space)
	DWORD dwThreadID; // thread ID (-1 = caller thread)
	DWORD dwFlags;    // reserved for future use, must be zero
};

static void SetThreadName(DWORD dwThreadID, LPCSTR szThreadName)
{
#ifdef _MSC_VER
	THREADNAME_INFO info;
	info.dwType = 0x1000;
	info.szName = szThreadName;
	info.dwThreadID = dwThreadID;
	info.dwFlags = 0;

	__try
	{
		RaiseException(0x406D1388, 0, sizeof(info) / sizeof(DWORD),
			(ULONG_PTR*)&info);
	}
	__except (EXCEPTION_CONTINUE_EXECUTION)
	{
	}
#else
	UNUSED(dwThreadID);
	UNUSED(szThreadName);
#endif
}

///////////////////////////////////////////////////////////////////////////
//
// Mutex
//
///////////////////////////////////////////////////////////////////////////

BCMutex::BCMutex(void)
{
#ifdef MUTEX_USE_CRITICAL_SECTION
	InitializeCriticalSection(&m_lock);
#else // MUTEX_USE_CRITICAL_SECTION
	InitializeSRWLock(&m_lock);
#endif // MUTEX_USE_CRITICAL_SECTION
}

BCMutex::~BCMutex(void)
{
#ifdef MUTEX_USE_CRITICAL_SECTION
	DeleteCriticalSection(&m_lock);
#else // MUTEX_USE_CRITICAL_SECTION
#endif // MUTEX_USE_CRITICAL_SECTION
}

void BCMutex::Lock(void)
{
#ifdef MUTEX_USE_CRITICAL_SECTION
	EnterCriticalSection(&m_lock);
#else // MUTEX_USE_CRITICAL_SECTION
	::AcquireSRWLockExclusive(reinterpret_cast<PSRWLOCK>(&m_lock));
#endif // MUTEX_USE_CRITICAL_SECTION
}

void BCMutex::Unlock(void)
{
#ifdef MUTEX_USE_CRITICAL_SECTION
	LeaveCriticalSection(&m_lock);
#else // MUTEX_USE_CRITICAL_SECTION
	::ReleaseSRWLockExclusive(reinterpret_cast<PSRWLOCK>(&m_lock));
#endif // MUTEX_USE_CRITICAL_SECTION
}



///////////////////////////////////////////////////////////////////////////
//
// Condition variable
//
///////////////////////////////////////////////////////////////////////////


//
// Condition variables are tricky to implement using NT synchronisation
// primitives, since none of them have the atomic "release mutex and Wait to be
// signalled" which is central to the idea of a condition variable.  To get
// around this the solution is to record which threads are waiting and
// explicitly wake up those threads.
//
// Here we implement a condition variable using a list of waiting threads
// (protected by a critical section), and a per-thread semaphore (which
// actually only needs to be a binary semaphore).
//
// To wait on the cv, a thread puts itself on the list of waiting threads for
// that cv, then releases the mutex and waits on its own personal semaphore.  A
// signalling thread simply takes a thread from the head of the list and kicks
// that thread's semaphore.  Broadcast is simply implemented by kicking the
// semaphore of each waiting thread.
//
// The only other tricky part comes when a thread gets a timeout from a timed
// Wait on its semaphore.  Between returning with a timeout from the Wait and
// entering the critical section, a signalling thread could get in, kick the
// waiting thread's semaphore and remove it from the list.  If this happens,
// the waiting thread's semaphore is now out of step so it needs resetting, and
// the thread should indicate that it was signalled rather than that it timed
// out.
//
// It is possible that the thread calling Wait or TimedWait is not a
// BCThread. In this case we have to provide a temporary data structure,
// i.e. for the duration of the call, for the thread to link itself on the
// list of waiting threads. _Internal_BCThread_Dummy provides such
// a data structure and _Internal_BCThread_Helper is a helper class to
// deal with this special case for Wait() and TimedWait(). Once created,
// the _Internal_BCThread_Dummy is cached for use by the next Wait() or
// TimedWait() call from a non-BCThread. This is probably worth doing
// because creating a Semaphore is quite heavy weight.

BCCondition::BCCondition(BCMutex* m) : m_pMutex(m)
{
	InitializeConditionVariable(&m_cv);
}


BCCondition::~BCCondition(void)
{
	//
}


void BCCondition::Wait(void)
{
	if (!SleepConditionVariable(&m_cv, &m_pMutex->m_lock, 
		INFINITE CV_SLEEP_FLAGS)) {
		// On failure, we only expect the CV to timeout. Any other error value means
		// that we've unexpectedly woken up.
		// Note that WAIT_TIMEOUT != ERROR_TIMEOUT. WAIT_TIMEOUT is used with the
		// WaitFor* family of functions as a direct return value. ERROR_TIMEOUT is
		// used with GetLastError().
		ASSERT(static_cast<DWORD>(ERROR_TIMEOUT) == GetLastError());
	}
}


int BCCondition::Wait(
	unsigned long rela_sec,
	unsigned long rela_nsec)
{
	DWORD timeout = rela_sec * 1000 + rela_nsec / 1000000;
	if (!SleepConditionVariable(&m_cv, &m_pMutex->m_lock, 
		timeout CV_SLEEP_FLAGS)) {
		// On failure, we only expect the CV to timeout. Any other error value means
		// that we've unexpectedly woken up.
		// Note that WAIT_TIMEOUT != ERROR_TIMEOUT. WAIT_TIMEOUT is used with the
		// WaitFor* family of functions as a direct return value. ERROR_TIMEOUT is
		// used with GetLastError().
		ASSERT(static_cast<DWORD>(ERROR_TIMEOUT) == GetLastError());
	}
	return true;
}

BCRESULT
BCCondition::TimedWait(BCTimeS *pTime)
{
	uint64_t microseconds;
	BCTimeS now;

	if (bc_time_now(&now) != BC_R_SUCCESS)
	{
		/* XXX */
		return (BC_R_UNEXPECTED);
	}

	microseconds = bc_time_microdiff(pTime, &now);
	if (!SleepConditionVariable(&m_cv, &m_pMutex->m_lock, 
		microseconds/1000 CV_SLEEP_FLAGS)) {
		// On failure, we only expect the CV to timeout. Any other error value means
		// that we've unexpectedly woken up.
		// Note that WAIT_TIMEOUT != ERROR_TIMEOUT. WAIT_TIMEOUT is used with the
		// WaitFor* family of functions as a direct return value. ERROR_TIMEOUT is
		// used with GetLastError().
		ASSERT(static_cast<DWORD>(ERROR_TIMEOUT) == GetLastError());
	}
	return BC_R_SUCCESS;
}


int BCCondition::TimedWait(
	unsigned long abs_sec,
	unsigned long abs_nsec)
{
	unsigned long now_sec, now_nsec;

	get_time_now(&now_sec, &now_nsec);

	DWORD timeout = (abs_sec * 1000 + abs_nsec / 1000000) 
		- (now_sec * 1000 + now_nsec / 1000000);

	if ((abs_sec <= now_sec) && ((abs_sec < now_sec) || (abs_nsec < now_nsec)))
		timeout = 0;

	if (!SleepConditionVariable(&m_cv, &m_pMutex->m_lock, 
		timeout CV_SLEEP_FLAGS)) {
		// On failure, we only expect the CV to timeout. Any other error value means
		// that we've unexpectedly woken up.
		// Note that WAIT_TIMEOUT != ERROR_TIMEOUT. WAIT_TIMEOUT is used with the
		// WaitFor* family of functions as a direct return value. ERROR_TIMEOUT is
		// used with GetLastError().
		ASSERT(static_cast<DWORD>(ERROR_TIMEOUT) == GetLastError());
	}
	return true;
}


void BCCondition::Signal(void)
{
	WakeConditionVariable(&m_cv);
}


void BCCondition::Broadcast(void)
{
	WakeAllConditionVariable(&m_cv);
}

///////////////////////////////////////////////////////////////////////////
//
// Counting semaphore
//
///////////////////////////////////////////////////////////////////////////

#define SEMAPHORE_MAX 0x7fffffff

BCSemaphore::BCSemaphore(uint32_t initial)
{
	m_ntSem = CreateSemaphore(NULL, initial, SEMAPHORE_MAX, NULL);

	if (m_ntSem == NULL)
	{
		DB( cerr << "BCSemaphore::BCSemaphore: CreateSemaphore error "
		         << GetLastError() << endl );
		throw BCThreadFatal(GetLastError());
	}
}


BCSemaphore::~BCSemaphore(void)
{
	if (m_ntSem != NULL)
	{
		CloseHandle(m_ntSem);
		m_ntSem = NULL;
	}
}


void BCSemaphore::Wait(void)
{
	if (WaitForSingleObject(m_ntSem, INFINITE) != WAIT_OBJECT_0)
		throw BCThreadFatal(GetLastError());
}


int BCSemaphore::TryWait(void)
{
	switch (WaitForSingleObject(m_ntSem, 0))
	{
	case WAIT_OBJECT_0:
		return 1;
	case WAIT_TIMEOUT:
		return 0;
	}

	throw BCThreadFatal(GetLastError());
	return 0; /* keep msvc++ happy */
}


void BCSemaphore::Post(void)
{
	if (!ReleaseSemaphore(m_ntSem, 1, NULL))
		throw BCThreadFatal(GetLastError());
}


///////////////////////////////////////////////////////////////////////////////
// Static helper methods
///////////////////////////////////////////////////////////////////////////////

static HANDLE Create(
    LPSECURITY_ATTRIBUTES lpEventAttributes,
    bool bManualReset,
    bool bInitialState,
    LPCTSTR lpName);

///////////////////////////////////////////////////////////////////////////////
// BCSysEvent
///////////////////////////////////////////////////////////////////////////////

BCSysEvent::BCSysEvent(
    LPSECURITY_ATTRIBUTES lpEventAttributes,
    bool bManualReset,
    bool bInitialState)
		:  m_hEvent(Create(lpEventAttributes, bManualReset, bInitialState, 0))
{

}

BCSysEvent::BCSysEvent(
    LPSECURITY_ATTRIBUTES lpEventAttributes,
    bool bManualReset,
    bool bInitialState,
    LPCTSTR lpszName)
		:  m_hEvent(Create(lpEventAttributes, bManualReset, bInitialState, lpszName))
{

}

BCSysEvent::~BCSysEvent()
{
	::CloseHandle(m_hEvent);
}

HANDLE BCSysEvent::GetEvent() const
{
	return m_hEvent;
}

void BCSysEvent::Wait() const
{
	if (!Wait(INFINITE))
	{
		throw BCException("BCSysEvent::Wait()",	"Unexpected timeout on infinite wait");
	}
}

bool BCSysEvent::Wait(DWORD timeoutMillis) const
{
	bool ok;

	DWORD result = ::WaitForSingleObject(m_hEvent, timeoutMillis);

	if (result == WAIT_TIMEOUT)
	{
		ok = false;
	}
	else if (result == WAIT_OBJECT_0)
	{
		ok = true;
	}
	else
	{
		char szError[32];
		snprintf(szError, sizeof(szError), "code[%d]", ::GetLastError());
		throw BCException("BCSysEvent::Wait() - WaitForSingleObject", szError);
	}

	return ok;
}

void BCSysEvent::Reset()
{
	if (!::ResetEvent(m_hEvent))
	{
		char szError[32];
		snprintf(szError, sizeof(szError), "code[%d]", ::GetLastError());
		throw BCException("BCSysEvent::Reset()", szError);
	}
}

void BCSysEvent::Set()
{
	if (!::SetEvent(m_hEvent))
	{
		char szError[32];
		snprintf(szError, sizeof(szError), "code[%d]", ::GetLastError());
		throw BCException("BCSysEvent::Set()", szError);
	}
}

void BCSysEvent::Pulse()
{
	if (!::PulseEvent(m_hEvent))
	{
		char szError[32];
		snprintf(szError, sizeof(szError), "code[%d]", ::GetLastError());
		throw BCException("BCSysEvent::Pulse()", szError);
	}
}

///////////////////////////////////////////////////////////////////////////////
// Static helper methods
///////////////////////////////////////////////////////////////////////////////

static HANDLE Create(
    LPSECURITY_ATTRIBUTES lpEventAttributes,
    bool bManualReset,
    bool bInitialState,
    LPCTSTR lpName)
{
	HANDLE hEvent = ::CreateEvent(lpEventAttributes, bManualReset, bInitialState, lpName);

	if (hEvent == NULL)
	{
		char szError[32];
		snprintf(szError, sizeof(szError), "code[%d]", ::GetLastError());
		throw BCException("BCSysEvent::Create()", szError);
	}

	return hEvent;
}

///////////////////////////////////////////////////////////////////////////////
// BCManEvent
///////////////////////////////////////////////////////////////////////////////

BCManEvent::BCManEvent(
    bool initialState /* = false */)
		:  BCSysEvent(0, true, initialState)
{

}

BCManEvent::BCManEvent(
    LPCTSTR lpszName,
    bool initialState /* = false */)
		:  BCSysEvent(0, true, initialState, lpszName)
{

}

///////////////////////////////////////////////////////////////////////////
//
// Thread
//
///////////////////////////////////////////////////////////////////////////

//
// Static variables
//

int					BCThread::init_t::count = 0;

BCMutex	*			BCThread::m_pNextIdMutex;
int					BCThread::m_nNextId = 0;
BCThread::init_t	BCThread::m_thread_init;

static DWORD self_tls_index;

//
// Initialisation function (gets called before any user code).
//

BCThread::init_t::init_t(void)
{
	if (count++ != 0)	// only do it once however many objects get created.
		return;

	DB(cerr << "BCThread::init: NT implementation initialising\n");

	self_tls_index = TlsAlloc();

	if (self_tls_index == 0xffffffff)
		throw BCThreadFatal(GetLastError());

	m_pNextIdMutex = new BCMutex;

	//
	// Create object for this (i.e. initial) thread.
	//

	BCThread* t = new BCThread;

	t->m_state = STATE_RUNNING;

	if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
	                     GetCurrentProcess(), &t->handle,
	                     0, FALSE, DUPLICATE_SAME_ACCESS))
		throw BCThreadFatal(GetLastError());

	t->nt_id = GetCurrentThreadId();

	DB(cerr << "initial thread " << t->id() << " NT thread id " << t->nt_id
	        << endl);

	if (!TlsSetValue(self_tls_index, (LPVOID)t))
		throw BCThreadFatal(GetLastError());

	if (!SetThreadPriority(t->handle, nt_priority(PRIORITY_NORMAL)))
		throw BCThreadFatal(GetLastError());
}

//
// Wrapper for thread creation.
//

extern "C"
unsigned __stdcall BCThreadWrapper(void* ptr)
{
	BCThread* me = (BCThread*)ptr;

	DB(cerr << "BCThreadWrapper: thread " << me->id()
	        << " started\n");

	if (!TlsSetValue(self_tls_index, (LPVOID)me))
	{
		throw BCThreadFatal(GetLastError());
	}

	if (!me->m_strName.empty())
	{
		SetThreadName(-1, me->m_strName.c_str());
	}

	//
	// Now invoke the thread function with the given argument.
	//

	if (me->fn_void != NULL)
	{
		(*me->fn_void)(me->m_pThreadArg);
		BCThread::Exit();
	}

	if (me->fn_ret != NULL)
	{
		void* return_value = (*me->fn_ret)(me->m_pThreadArg);
		BCThread::Exit(return_value);
	}

	if (me->m_bDetached)
	{
		me->Run(me->m_pThreadArg);
		BCThread::Exit();
	}
	else
	{
		void* return_value = me->RunUndetached(me->m_pThreadArg);
		BCThread::Exit(return_value);
	}

	// should never get here.
	return 0;
}


//
// Constructors for BCThread - set up the thread object but don't
// start it running.
//

// construct a m_bDetached thread running a given function.

BCThread::BCThread(
	void (*fn)(void*), 
	void* arg, 
	priority_t pri,
	LPCSTR lpszThreadName)
	: m_strName(lpszThreadName)
{
	CommonConstructor(arg, pri, 1);
	fn_void = fn;
	fn_ret = NULL;
}

// construct an undetached thread running a given function.

BCThread::BCThread(
	void* (*fn)(void*), 
	void* arg, 
	priority_t pri,
	LPCSTR lpszThreadName)
	: m_strName(lpszThreadName)
{
	CommonConstructor(arg, pri, 0);
	fn_void = NULL;
	fn_ret = fn;
}

// construct a thread which will run either Run() or RunUndetached().

BCThread::BCThread(void* arg, priority_t pri)
{
	CommonConstructor(arg, pri, 1);
	fn_void = NULL;
	fn_ret = NULL;
}

// common part of all constructors.

void BCThread::CommonConstructor(void* arg, priority_t pri, bool det)
{
	m_state = STATE_NEW;
	m_priority = pri;

	m_pNextIdMutex->Lock();
	m_nId = m_nNextId++;
	m_pNextIdMutex->Unlock();

	m_pThreadArg = arg;
	m_bDetached = det;	// may be altered in StartUndetached()

	handle = NULL;
}


//
// Destructor for BCThread.
//

BCThread::~BCThread(void)
{
	DB(cerr << "destructor called for thread " << id() << endl);
	if (handle != NULL) {
		CloseHandle(handle);
		handle = NULL;
	}
}


//
// Start the thread
//

void BCThread::Start(void)
{
	BCMutex::Owner l(m_Mutex);

	if (m_state != STATE_NEW)
		throw BCThreadInvalid();

	unsigned int t;
	handle = (HANDLE)_beginthreadex(
	             NULL,
	             0,
	             BCThreadWrapper,
	             (LPVOID)this,
	             CREATE_SUSPENDED,
	             &t);
	nt_id = t;
	if (handle == NULL)
		throw BCThreadFatal(GetLastError());

	if (!SetThreadPriority(handle, m_priority))
		throw BCThreadFatal(GetLastError());

	if (ResumeThread(handle) == 0xffffffff)
		throw BCThreadFatal(GetLastError());

	m_state = STATE_RUNNING;
}


//
// Start a thread which will run the member function RunUndetached().
//

void BCThread::StartUndetached(void)
{
	if ((fn_void != NULL) || (fn_ret != NULL))
		throw BCThreadInvalid();

	m_bDetached = 0;
	Start();
}


//
// Join - simply check error conditions & call WaitForSingleObject.
//

void BCThread::Join(void** status)
{
	m_Mutex.Lock();

	if ((m_state != STATE_RUNNING) && (m_state != STATE_TERMINATED))
	{
		m_Mutex.Unlock();
		throw BCThreadInvalid();
	}

	m_Mutex.Unlock();

	if (this == Self())
		throw BCThreadInvalid();

	if (m_bDetached)
		throw BCThreadInvalid();

	DB(cerr << "BCThread::Join: doing WaitForSingleObject\n");

	if (WaitForSingleObject(handle, INFINITE) != WAIT_OBJECT_0)
		throw BCThreadFatal(GetLastError());

	DB(cerr << "BCThread::Join: WaitForSingleObject succeeded\n");

	if (status)
		*status = return_val;

	delete this;
}


//
// Change this thread's priority.
//

void BCThread::SetPriority(priority_t pri)
{
	BCMutex::Owner l(m_Mutex);

	if (m_state != STATE_RUNNING)
		throw BCThreadInvalid();

	m_priority = pri;

	if (!SetThreadPriority(handle, nt_priority(pri)))
		throw BCThreadFatal(GetLastError());
}

//
// Check this thread's exit code.
//

bool BCThread::IsRunning() const
{
	if (handle)
	{
		DWORD ec = 0;
		return GetExitCodeThread(handle, &ec) && ec == STILL_ACTIVE;
	}
	return false;
}


//
// Create - construct a new thread object and start it running.  Returns thread
// object if successful, null pointer if not.
//

// m_bDetached version

BCThread* BCThread::Create(void (*fn)(void*), void* arg, priority_t pri)
{
	BCThread* t = new BCThread(fn, arg, pri);
	t->Start();
	return t;
}

// undetached version

BCThread* BCThread::Create(void* (*fn)(void*), void* arg, priority_t pri)
{
	BCThread* t = new BCThread(fn, arg, pri);
	t->Start();
	return t;
}


//
// Exit() _must_ Lock the mutex even in the case of a m_bDetached thread.  This is
// because a thread may run to completion before the thread that created it has
// had a chance to get out of Start().  By locking the mutex we ensure that the
// creating thread must have reached the end of Start() before we delete the
// thread object.  Of course, once the call to Start() returns, the user can
// still incorrectly refer to the thread object, but that's their problem.
//

void BCThread::Exit(void* return_value)
{
	BCThread* me = Self();

	if (me)
	{
		me->m_Mutex.Lock();

		me->m_state = STATE_TERMINATED;

		me->m_Mutex.Unlock();

		DB(cerr << "BCThread::Exit: thread " << me->id() << " m_bDetached "
		        << me->m_bDetached << " return value " << return_value << endl);

		if (me->m_bDetached)
		{
			delete me;
		}
		else
		{
			me->return_val = return_value;
		}
	}
	else
	{
		DB(cerr << "BCThread::Exit: called with a non-nwthread. Exit quietly." << endl);
	}
	//   _endthreadex() does not automatically closes the thread handle.
	//   The BCThread dtor closes the thread handle.
	_endthreadex(0);
}


BCThread* BCThread::Self(void)
{
	LPVOID me;

	me = TlsGetValue(self_tls_index);

	if (me == NULL)
	{
		DB(cerr << "BCThread::Self: called with a non-ominthread. NULL is returned." << endl);
	}
	return (BCThread*)me;
}


void BCThread::yield(void)
{
	::Sleep(0);
}


#define MAX_SLEEP_SECONDS (DWORD)4294966	// (2**32-2)/1000

void BCThread::Sleep(unsigned long secs, unsigned long nanosecs)
{
	if (secs <= MAX_SLEEP_SECONDS)
	{
		::Sleep(secs * 1000 + nanosecs / 1000000);
		return;
	}

	DWORD no_of_max_sleeps = secs / MAX_SLEEP_SECONDS;

	for (DWORD i = 0; i < no_of_max_sleeps; i++)
	{
		::Sleep(MAX_SLEEP_SECONDS * 1000);
	}

	::Sleep((secs % MAX_SLEEP_SECONDS) * 1000 + nanosecs / 1000000);
}


void BCThread::GetTime(unsigned long* abs_sec, unsigned long* abs_nsec,
                      unsigned long rel_sec, unsigned long rel_nsec)
{
	get_time_now(abs_sec, abs_nsec);
	*abs_nsec += rel_nsec;
	*abs_sec += rel_sec + *abs_nsec / 1000000000;
	*abs_nsec = *abs_nsec % 1000000000;
}


int BCThread::nt_priority(priority_t pri)
{
	switch (pri)
	{
	case PRIORITY_LOW:
		return THREAD_PRIORITY_LOWEST;

	case PRIORITY_NORMAL:
		return THREAD_PRIORITY_NORMAL;

	case PRIORITY_HIGH:
		return THREAD_PRIORITY_HIGHEST;
	}

	throw BCThreadInvalid();
	return 0; /* keep msvc++ happy */
}


static void get_time_now(unsigned long* abs_sec, unsigned long* abs_nsec)
{
	static int days_in_preceding_months[12]
	= { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
	static int days_in_preceding_months_leap[12]
	= { 0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335 };

	SYSTEMTIME st;

	GetSystemTime(&st);
	*abs_nsec = st.wMilliseconds * 1000000;

	// this formula should work until 1st March 2100

	DWORD days = ((st.wYear - 1970) * 365 + (st.wYear - 1969) / 4
	              + ((st.wYear % 4)
	                 ? days_in_preceding_months[st.wMonth - 1]
	                 : days_in_preceding_months_leap[st.wMonth - 1])
	              + st.wDay - 1);

	*abs_sec = st.wSecond + 60 * (st.wMinute + 60 * (st.wHour + 24 * days));
}


///////////////////////////////////////////////////////////////////////////////
// Namespace: BC
///////////////////////////////////////////////////////////////////////////////

} // End of namespace BC


///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////