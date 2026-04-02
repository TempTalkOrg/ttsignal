
#ifndef BC_WIN32_BCTHREAD_H_INCLUDED__
#define BC_WIN32_BCTHREAD_H_INCLUDED__


#ifndef _WINDOWS_
#include <windows.h>
#endif

#include "BC/Exports.h"
#include <BC/BCTime.h>
#include <string>


///////////////////////////////////////////////////////////////////////////////
// Namespace: BC
///////////////////////////////////////////////////////////////////////////////

namespace BC
{

extern "C" unsigned __stdcall BCThreadWrapper(LPVOID ptr);


///////////////////////////////////////////////////////////////////////////////
// BCThread
///////////////////////////////////////////////////////////////////////////////

class BCMutex;
class BCCondition;
class BCSemaphore;
class BCThread;


//
// This exception is thrown in the event of a fatal error.
//

class BC_API BCThreadFatal
{
public:
	int32_t				m_dError;
	BCThreadFatal(int32_t e = 0)
		: m_dError(e)
	{}
};


//
// This exception is thrown when an operation is invoked with invalid
// arguments.
//

class BC_API BCThreadInvalid {};


///////////////////////////////////////////////////////////////////////////
//
// BCMutex
//
///////////////////////////////////////////////////////////////////////////

class BC_API BCMutex
{
public:
	class BC_API Owner
	{
		BCMutex			&m_mutex;
	public:
		Owner(BCMutex& m) : m_mutex(m)
		{
			m_mutex.Lock();
		}
		~Owner(void)
		{
			m_mutex.Unlock();
		}
	private:
		// dummy copy constructor and operator= to prevent copying
		DECLARE_NO_COPY_CLASS(Owner)
	};

public:
	BCMutex(void);
	~BCMutex(void);

	void Lock(void);
	void Unlock(void);
	void Acquire(void)
	{
		Lock();
	}
	void Release(void)
	{
		Unlock();
	}
	// the names Lock and Unlock are preferred over Acquire and release
	// since we are attempting to be as POSIX-like as possible.

	friend class BCCondition;

private:
	// dummy copy constructor and operator= to prevent copying
	DECLARE_NO_COPY_CLASS(BCMutex)

private:
#ifdef MUTEX_USE_CRITICAL_SECTION
	CRITICAL_SECTION			m_lock;
#else // MUTEX_USE_CRITICAL_SECTION
	SRWLOCK						m_lock;
#endif // MUTEX_USE_CRITICAL_SECTION
};


///////////////////////////////////////////////////////////////////////////
//
// Condition variable
//
///////////////////////////////////////////////////////////////////////////

class BC_API BCCondition
{
public:
	BCCondition(BCMutex* m);
	// constructor must be given a pointer to an existing mutex. The
	// condition variable is then linked to the mutex, so that there is an
	// implicit Unlock and Lock around Wait() and timed_wait().

	~BCCondition(void);

	void Wait(void);
	// wait for the condition variable to be signalled.  The mutex is
	// implicitly released before waiting and locked again after waking up.
	// If Wait() is called by multiple threads, a Signal may wake up more
	// than one thread.  See POSIX threads documentation for details.

	int32_t Wait(unsigned long secs, unsigned long nanosecs = 0);
	// Wait is given an relative time to wait until.
	// Returns 1 (true) if successfully signalled, 0 (false) if time
	// expired.

	BCRESULT	TimedWait(BCTimeS *pTime);

	int32_t TimedWait(unsigned long secs, unsigned long nanosecs = 0);
	// TimedWait() is given an absolute time to wait until.  To wait for a
	// relative time from now, use BCThread::GetTime. See POSIX threads
	// documentation for why absolute times are better than relative.
	// Returns 1 (true) if successfully signalled, 0 (false) if time
	// expired.

	void Signal(void);
	// if one or more threads have called Wait(), Signal wakes up at least
	// one of them, possibly more.  See POSIX threads documentation for
	// details.

	void Broadcast(void);
	// Broadcast is like Signal but wakes all threads which have called
	// Wait().

private:
	// dummy copy constructor and operator= to prevent copying
	DECLARE_NO_COPY_CLASS(BCCondition)

private:
	CONDITION_VARIABLE			m_cv;
	BCMutex					*	m_pMutex;
};


///////////////////////////////////////////////////////////////////////////
//
// Counting semaphore
//
///////////////////////////////////////////////////////////////////////////

class BC_API BCSemaphore
{
public:
	//
	// A helper class for semaphores, similar to Owner above.
	//

	class BC_API Owner
	{
		BCSemaphore			&m_sem;
	public:
		Owner(BCSemaphore& s) : m_sem(s)
		{
			m_sem.Wait();
		}
		~Owner(void)
		{
			m_sem.Post();
		}
	private:
		// dummy copy constructor and operator= to prevent copying
		DECLARE_NO_COPY_CLASS(Owner)
	};

public:
	BCSemaphore(uint32_t initial = 1);
	~BCSemaphore(void);

	void Wait(void);
	// if semaphore value is > 0 then decrement it and carry on. If it's
	// already 0 then block.

	int32_t TryWait(void);
	// if semaphore value is > 0 then decrement it and return 1 (true).
	// If it's already 0 then return 0 (false).

	void Post(void);
	// if any threads are blocked in Wait(), wake one of them up. Otherwise
	// increment the value of the semaphore.

private:
	// dummy copy constructor and operator= to prevent copying
	DECLARE_NO_COPY_CLASS(BCSemaphore)

private:
	HANDLE					m_ntSem;
};

///////////////////////////////////////////////////////////////////////////////
// BCSysEvent
///////////////////////////////////////////////////////////////////////////////

class BC_API BCSysEvent
{
public :

	BCSysEvent(
	    LPSECURITY_ATTRIBUTES lpSecurityAttributes,
	    bool manualReset,
	    bool initialState);

	BCSysEvent(
	    LPSECURITY_ATTRIBUTES lpSecurityAttributes,
	    bool manualReset,
	    bool initialState,
	    LPCTSTR lpszName);

	virtual ~BCSysEvent();

	HANDLE GetEvent() const;

	void Wait() const;

	bool Wait(
	    DWORD timeoutMillis) const;

	void Reset();

	void Set();

	void Pulse();

private :
	DECLARE_NO_COPY_CLASS(BCSysEvent);
	HANDLE m_hEvent;
};

///////////////////////////////////////////////////////////////////////////////
// BCManEvent
///////////////////////////////////////////////////////////////////////////////

class BC_API BCManEvent : public BCSysEvent
{
public :

	explicit BCManEvent(
		bool initialState = false);

	explicit BCManEvent(
		LPCTSTR lpszName,
		bool initialState = false);

private :
	DECLARE_NO_COPY_CLASS(BCManEvent);
};

///////////////////////////////////////////////////////////////////////////
//
// Thread
//
///////////////////////////////////////////////////////////////////////////

class BC_API BCThread
{
public:
	enum priority_t
	{
		PRIORITY_LOW,
		PRIORITY_NORMAL,
		PRIORITY_HIGH
	};

	enum state_t
	{
		STATE_NEW,		// thread object exists but thread hasn't
		// started yet.
		STATE_RUNNING,		// thread is running.
		STATE_TERMINATED	// thread has terminated but storage has not
		// been reclaimed (i.e. waiting to be joined).
	};

	//
	// Constructors set up the thread object but the thread won't start until
	// Start() is called. The Create method can be used to construct and Start
	// a thread in a single call.
	//

	BCThread(
		void (*fn)(void*), 
		void* arg = NULL,
		priority_t pri = PRIORITY_NORMAL,
		LPCSTR lpszThreadName = ""
	);
	BCThread(
		void* (*fn)(void*), 
		void* arg = NULL,
		priority_t pri = PRIORITY_NORMAL,
		LPCSTR lpszThreadName = ""
	);
	// these constructors Create a thread which will run the given function
	// when Start() is called.  The thread will be m_bDetached if given a
	// function with void return type, undetached if given a function
	// returning void*. If a thread is m_bDetached, storage for the thread is
	// reclaimed automatically on termination. Only an undetached thread
	// can be joined.

	void Start(void);
	// Start() causes a thread created with one of the constructors to
	// start executing the appropriate function.

protected:

	BCThread(void* arg = NULL, priority_t pri = PRIORITY_NORMAL);
	// this constructor is used in a derived class.  The thread will
	// execute the Run() or RunUndetached() member functions depending on
	// whether Start() or StartUndetached() is called respectively.

	void StartUndetached(void);
	// can be used with the above constructor in a derived class to cause
	// the thread to be undetached.  In this case the thread executes the
	// RunUndetached member function.

	virtual ~BCThread(void);
	// destructor cannot be called by user (except via a derived class).
	// Use Exit() or Cancel() instead. This also means a thread object must
	// be allocated with new - it cannot be statically or automatically
	// allocated. The destructor of a class that inherits from BCThread
	// shouldn't be public either (otherwise the thread object can be
	// destroyed while the underlying thread is still running).

public:

	void Join(void**);
	// Join causes the calling thread to Wait for another's completion,
	// putting the return value in the variable of type void* whose address
	// is given (unless passed a null pointer). Only undetached threads
	// may be joined. Storage for the thread will be reclaimed.

	void SetPriority(priority_t);
	// set the priority of the thread.

	bool IsRunning() const;
	// check thread exit code

	static BCThread* Create(void (*fn)(void*), void* arg = NULL,
	                        priority_t pri = PRIORITY_NORMAL);
	static BCThread* Create(void* (*fn)(void*), void* arg = NULL,
	                        priority_t pri = PRIORITY_NORMAL);
	// Create spawns a new thread executing the given function with the
	// given argument at the given priority. Returns a pointer to the
	// thread object. It simply constructs a new thread object then calls
	// start.

	static void Exit(void* return_value = NULL);
	// causes the calling thread to terminate.

	static BCThread* Self(void);
	// returns the calling thread's BCThread object.
	// If the calling thread is not the main thread and
	// is not created using this library, returns 0.

	static void yield(void);
	// allows another thread to run.

	static void Sleep(unsigned long secs, unsigned long nanosecs = 0);
	// sleeps for the given time.

	static void GetTime(unsigned long* abs_sec, unsigned long* abs_nsec,
	                     unsigned long rel_sec = 0, unsigned long rel_nsec=0);
	// calculates an absolute time in seconds and nanoseconds, suitable for
	// use in timed_waits on condition variables, which is the current time
	// plus the given relative offset.

private:

	virtual void Run(void* arg) {}
	virtual void* RunUndetached(void* arg)
	{
		return NULL;
	}
	// can be overridden in a derived class.  When constructed using the
	// the constructor BCThread(void*, priority_t), these functions are
	// called by Start() and StartUndetached() respectively.

	void CommonConstructor(void* arg, priority_t pri, bool det);
	// implements the common parts of the constructors.

	BCMutex					m_Mutex;
	// used to protect any members which can change after construction,
	// i.e. the following 2 members:

	state_t					m_state;
	priority_t				m_priority;

	static BCMutex			*m_pNextIdMutex;
	static int32_t			m_nNextId;
	int32_t					m_nId;

	void (*fn_void)(void*);
	void* (*fn_ret)(void*);
	void					*m_pThreadArg;
	bool					m_bDetached;
	std::string				m_strName;

public:

	priority_t priority(void)
	{
		// return this thread's priority.

		BCMutex::Owner l(m_Mutex);
		return m_priority;
	}

	state_t state(void)
	{
		// return thread state (invalid, new, running or terminated).

		BCMutex::Owner l(m_Mutex);
		return m_state;
	}

	int32_t id(void)
	{
		return m_nId;
	}
	// return unique thread id within the current process.


	// This class plus the instance of it declared below allows us to execute
	// some initialisation code before main() is called.

	class BC_API init_t
	{
		static int32_t count;
	public:
		init_t(void);
	};

	friend class init_t;

private:
	HANDLE					handle;
	DWORD					nt_id;
	void					*return_val;

	static	init_t			m_thread_init;

	static int32_t nt_priority(priority_t);

	friend class BCCondition;
	friend unsigned __stdcall BCThreadWrapper(LPVOID ptr);
};

///////////////////////////////////////////////////////////////////////////////
// Namespace: BC
///////////////////////////////////////////////////////////////////////////////

} // End of namespace BC


#endif // BC_WIN32_BCTHREAD_H_INCLUDED__

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////