#ifndef BC_LINUX_BCTHREAD_H_INCLUDED__
#define BC_LINUX_BCTHREAD_H_INCLUDED__


#include <BC/Exports.h>
#include <BC/BCTime.h>
#include <pthread.h>
#include <string>

#ifndef NULL
#define NULL 0
#endif


//for config.h
#define __stdcall
// #define uint32_t unsigned int
// #define int32_t int
#define LPVOID void *
//for config.h end


//
// This exception is thrown in the event of a fatal error.
//
namespace BC
{

class BCMutex;
class BCCondition;
class BCSemaphore;
class BCThread;

extern "C" void* __stdcall BCThreadWrapper(LPVOID ptr);

class BC_API BCThreadFatal {
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
// Mutex
//
///////////////////////////////////////////////////////////////////////////

class BC_API BCMutex
{
public:
    BCMutex(void);
    ~BCMutex(void);

	class BC_API Owner
	{
		BCMutex& m_mutex;
	public:
		Owner(BCMutex& m) : m_mutex(m) { m_mutex.Lock(); }
		~Owner(void) { m_mutex.Unlock(); }
	private:
		Owner(const Owner&);
		Owner& operator=(const Owner&);
	};

    inline void Lock(void)    { pthread_mutex_lock(&posix_mutex);  }
    inline void Unlock(void)  { pthread_mutex_unlock(&posix_mutex); }
    inline void Acquire(void) { Lock(); }
    inline void Release(void) { Unlock(); }
        // the names lock and unlock are preferred over acquire and release
        // since we are attempting to be as POSIX-like as possible.

    friend class BCCondition;

private:
    // dummy copy constructor and operator= to prevent copying
    BCMutex(const BCMutex&);
    BCMutex& operator=(const BCMutex&);

private:
    pthread_mutex_t posix_mutex;
};




///////////////////////////////////////////////////////////////////////////
//
// Condition variable
//
///////////////////////////////////////////////////////////////////////////

class BC_API BCCondition {

    BCMutex* m_pMutex;

public:
    BCCondition(BCMutex* m);
        // constructor must be given a pointer to an existing mutex. The
        // condition variable is then linked to the mutex, so that there is an
        // implicit unlock and lock around wait() and timed_wait().

    ~BCCondition(void);

    void Wait(void);
        // wait for the condition variable to be signalled.  The mutex is
        // implicitly released before waiting and locked again after waking up.
        // If wait() is called by multiple threads, a signal may wake up more
        // than one thread.  See POSIX threads documentation for details.

    BCRESULT Wait(BCIntervalS *pTime);
        // Wait is given an relative time to wait until.
        // Returns 1 (true) if successfully signalled, 0 (false) if time
        // expired.

	BCRESULT TimedWait(BCTimeS *pTime);
        // timedwait() is given an absolute time to wait until.  To wait for a
        // relative time from now, use omni_thread::get_time. See POSIX threads
        // documentation for why absolute times are better than relative.
        // Returns 1 (true) if successfully signalled, 0 (false) if time
        // expired.


    int32_t TimedWait(unsigned long secs, unsigned long nanosecs = 0);
        // timedwait() is given an absolute time to wait until.  To wait for a
        // relative time from now, use omni_thread::get_time. See POSIX threads
        // documentation for why absolute times are better than relative.
        // Returns 1 (true) if successfully signalled, 0 (false) if time
        // expired.

    void Signal(void);
        // if one or more threads have called wait(), signal wakes up at least
        // one of them, possibly more.  See POSIX threads documentation for
        // details.

    void Broadcast(void);
        // broadcast is like signal but wakes all threads which have called
        // wait().

private:
    // dummy copy constructor and operator= to prevent copying
    BCCondition(const BCCondition&);
    BCCondition& operator=(const BCCondition&);

private:
    pthread_cond_t posix_cond;
};


///////////////////////////////////////////////////////////////////////////
//
// Counting semaphore
//
///////////////////////////////////////////////////////////////////////////

class BC_API BCSemaphore {
public:
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
		Owner(const Owner&);
		Owner& operator=(const Owner&);
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
        // if any threads are blocked in wait(), wake one of them up. Otherwise
        // increment the value of the semaphore.

private:
    // dummy copy constructor and operator= to prevent copying
    BCSemaphore(const BCSemaphore&);
    BCSemaphore& operator=(const BCSemaphore&);

private:
    BCMutex m;
    BCCondition c;
    int32_t value;
};

///////////////////////////////////////////////////////////////////////////////
// BCManEvent
///////////////////////////////////////////////////////////////////////////////

class BC_API BCManEvent
{
public :

	BCManEvent(bool initialState = false);

	virtual ~BCManEvent();

	void Wait();

	bool Wait(DWORD timeoutMillis) ;

	void Reset();

	void Set();

	void Pulse();

private :
	DECLARE_NO_COPY_CLASS(BCManEvent);
	BCMutex					m_sLock;
	BCCondition				m_cond;
};


///////////////////////////////////////////////////////////////////////////
//
// Thread
//
///////////////////////////////////////////////////////////////////////////

class BC_API BCThread 
{
public:
    enum priority_t {
        PRIORITY_LOW,
        PRIORITY_NORMAL,
        PRIORITY_HIGH
    };

    enum state_t {
        STATE_NEW,              // thread object exists but thread hasn't
                                // started yet.
        STATE_RUNNING,          // thread is running.
        STATE_TERMINATED        // thread has terminated but storage has not
                                // been reclaimed (i.e. waiting to be joined).
    };

    //
    // Constructors set up the thread object but the thread won't start until
    // start() is called. The create method can be used to construct and start
    // a thread in a single call.
    //

    BCThread(
        void (*fn)(void*), 
        void* arg = NULL,
        priority_t pri = PRIORITY_NORMAL,
        LPCSTR lpszThreadName = "");
    BCThread(
        void* (*fn)(void*), 
        void* arg = NULL,
        priority_t pri = PRIORITY_NORMAL,
        LPCSTR lpszThreadName = "");
        // these constructors create a thread which will run the given function
        // when start() is called.  The thread will be detached if given a
        // function with void return type, undetached if given a function
        // returning void*. If a thread is detached, storage for the thread is
        // reclaimed automatically on termination. Only an undetached thread
        // can be joined.

    void Start(void);
        // start() causes a thread created with one of the constructors to
        // start executing the appropriate function.

protected:

    BCThread(void* arg = NULL, priority_t pri = PRIORITY_NORMAL);
        // this constructor is used in a derived class.  The thread will
        // execute the run() or run_undetached() member functions depending on
        // whether start() or start_undetached() is called respectively.

    void StartUndetached(void);
        // can be used with the above constructor in a derived class to cause
        // the thread to be undetached.  In this case the thread executes the
        // run_undetached member function.

    virtual ~BCThread(void);
        // destructor cannot be called by user (except via a derived class).
        // Use exit() or cancel() instead. This also means a thread object must
        // be allocated with new - it cannot be statically or automatically
        // allocated. The destructor of a class that inherits from omni_thread
        // shouldn't be public either (otherwise the thread object can be
        // destroyed while the underlying thread is still running).

public:

    void Join(void**);
        // join causes the calling thread to wait for another's completion,
        // putting the return value in the variable of type void* whose address
        // is given (unless passed a null pointer). Only undetached threads
        // may be joined. Storage for the thread will be reclaimed.

    void SetPriority(priority_t);
        // set the priority of the thread.

    static BCThread* Create(void (*fn)(void*), void* arg = NULL,
                               priority_t pri = PRIORITY_NORMAL);
    static BCThread* Create(void* (*fn)(void*), void* arg = NULL,
                               priority_t pri = PRIORITY_NORMAL);
        // create spawns a new thread executing the given function with the
        // given argument at the given priority. Returns a pointer to the
        // thread object. It simply constructs a new thread object then calls
        // start.

    static void Exit(void* return_value = NULL);
        // causes the calling thread to terminate.

    static BCThread* Self(void);
        // returns the calling thread's omni_thread object.  If the
        // calling thread is not the main thread and is not created
        // using this library, returns 0. (But see create_dummy()
        // below.)

    static void Yield(void);
        // allows another thread to run.

    static void Sleep(unsigned long secs, unsigned long nanosecs = 0);
        // sleeps for the given time.

    static void GetTime(unsigned long* abs_sec, unsigned long* abs_nsec,
                         unsigned long rel_sec = 0, unsigned long rel_nsec=0);
        // calculates an absolute time in seconds and nanoseconds, suitable for
        // use in timed_waits on condition variables, which is the current time
        // plus the given relative offset.

//******************************************************************************************************************
    static void StackSize(unsigned long sz);
    static unsigned long StackSize();
        // Use this value as the stack size when spawning a new thread.
        // The default value (0) means that the thread library default is
        // to be used.


    // Per-thread data
    //
    // These functions allow you to attach additional data to an
    // omni_thread. First allocate a key for yourself with
    // allocate_key(). Then you can store any object whose class is
    // derived from value_t. Any values still stored in the
    // omni_thread when the thread exits are deleted.
    //
    // These functions are NOT thread safe, so you should be very
    // careful about setting/getting data in a different thread to the
    // current thread.

    typedef uint32_t key_t;
    static key_t allocate_key();

    class value_t {
    public:
      virtual ~value_t() {}
    };

    value_t* set_value(key_t k, value_t* v);
        // Sets a value associated with the given key. The key must
        // have been allocated with allocate_key(). If a value has
        // already been set with the specified key, the old value_t
        // object is deleted and replaced. Returns the value which was
        // set, or zero if the key is invalid.

    value_t* get_value(key_t k);
        // Returns the value associated with the key. If the key is
        // invalid, or there is no value for the key, returns zero.

    value_t* remove_value(key_t k);
        // Removes the value associated with the key and returns it.
        // If the key is invalid, or there is no value for the key,
        // returns zero.


    // Dummy omni_thread
    //
    // Sometimes, an application finds itself with threads created
    // outside of omnithread which must interact with omnithread
    // features such as the per-thread data. In this situation,
    // omni_thread::self() would normally return 0. These functions
    // allow the application to create a suitable dummy omni_thread
    // object.

    static BCThread* create_dummy(void);
        // creates a dummy omni_thread for the calling thread. Future
        // calls to self() will return the dummy omni_thread. Throws
        // omni_thread_invalid if this thread already has an
        // associated omni_thread (real or dummy).

    static void release_dummy();
        // release the dummy omni_thread for this thread. This
        // function MUST be called before the thread exits. Throws
        // omni_thread_invalid if the calling thread does not have a
        // dummy omni_thread.

    // class ensure_self should be created on the stack. If created in
    // a thread without an associated omni_thread, it creates a dummy
    // thread which is released when the ensure_self object is deleted.

    class ensure_self {
    public:
      inline ensure_self() : _dummy(0)
      {
        _self = BCThread::Self();
        if (!_self) {
          _dummy = 1;
          _self  = BCThread::create_dummy();
        }
      }
      inline ~ensure_self()
      {
        if (_dummy)
          BCThread::release_dummy();
      }
      inline BCThread* self() { return _self; }
    private:
      BCThread* _self;
      int32_t          _dummy;
    };
//*******************************************************************************************************************************

private:

    virtual void Run(void* arg) { UNUSED(arg); }
    virtual void* RunUndetached(void* arg)
	{
		UNUSED(arg);
		return NULL;
	}
        // can be overridden in a derived class.  When constructed using the
        // the constructor omni_thread(void*, priority_t), these functions are
        // called by start() and start_undetached() respectively.

    void CommonConstructor(void* arg, priority_t pri, int32_t det);
        // implements the common parts of the constructors.

    BCMutex             m_Mutex;
        // used to protect any members which can change after construction,
        // i.e. the following 2 members.

    state_t             m_state;
    priority_t          m_priority;
    int32_t             m_nId;

    void                (*fn_void)(void*);
    void            *   (*fn_ret)(void*);
    void            *   m_pThreadArg;
    bool                m_bDetached;
	//****************************************************************************************************************************
    int32_t             _dummy;
    value_t         **  _values;
    unsigned long       _value_alloc;
	//*****************************************************************************************************************************

    static BCMutex  *   m_pNextIdMutex;
    static int32_t      m_nNextId;

    BCThread(const BCThread&);
    BCThread& operator=(const BCThread&);
    // Not implemented

public:

    priority_t priority(void) {

        // return this thread's priority.

        BCMutex::Owner l(m_Mutex);
		return m_priority;
    }

    state_t state(void) {

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
		//static int32_t count;
	public:
		init_t(void);
		~init_t(void);
    };

    friend class init_t;
    friend class BCThreadDummy;


private:
    pthread_t posix_thread;
    static int32_t posix_priority(priority_t);

	friend class BCCondition;
	friend void* __stdcall BCThreadWrapper(LPVOID ptr);
	BCSemaphore		m_startSignal;
    std::string     m_strName;
};

}

#endif // BC_LINUX_BCTHREAD_H_INCLUDED__
