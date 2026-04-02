
#include <BC/Utils.h>
#include <BC/BCException.h>
#include <BC/BCThread.h>

#ifdef __GNUC__
#define HAVE_GETTIMEOFDAY	1
#endif

#ifdef HAVE_NANOSLEEP
#undef NoNanoSleep
#else
#define NoNanoSleep
#endif

#ifdef HAVE_SYS_TIME_H
// typedef of struct timeval and gettimeofday();
#include <sys/time.h>
#include <unistd.h>
#endif

#if defined(__linux__) && defined(_MIT_POSIX_THREADS)
#include <pthread/mit/sys/timers.h>
#endif

#if defined(__irix__) && defined(PthreadSupportThreadPriority)
#if _POSIX_THREAD_PRIORITY_SCHEDULING
#include <sched.h>
#endif
#endif

#define DB(x) // x
//#include <iostream.h> or #include <iostream> if DB is on.

#if (PthreadDraftVersion <= 6)
#define ERRNO(x) (((x) != 0) ? (errno) : 0)
#ifdef __VMS
// pthread_setprio returns old priority on success (draft version 4:
// OpenVms version < 7)
#define THROW_ERRORS(x) { if ((x) == -1) throw BCThreadFatal(errno); }
#else
#define THROW_ERRORS(x) { if ((x) != 0) throw BCThreadFatal(errno); }
#endif
#else
#define ERRNO(x) (x)
#define THROW_ERRORS(x) { int32_t rc = (x); \
                          if (rc != 0) throw BCThreadFatal(rc); }
#endif


namespace BC
{
///////////////////////////////////////////////////////////////////////////
//
// Mutex
//
///////////////////////////////////////////////////////////////////////////


BCMutex::BCMutex(void)
{
#if (PthreadDraftVersion == 4)
    THROW_ERRORS(pthread_mutex_init(&posix_mutex, pthread_mutexattr_default));
#else
    THROW_ERRORS(pthread_mutex_init(&posix_mutex, 0));
#endif
}

BCMutex::~BCMutex(void)
{
    THROW_ERRORS(pthread_mutex_destroy(&posix_mutex));
}


///////////////////////////////////////////////////////////////////////////
//
// Condition variable
//
///////////////////////////////////////////////////////////////////////////


BCCondition::BCCondition(BCMutex* m) : m_pMutex(m)
{
#if (PthreadDraftVersion == 4)
    THROW_ERRORS(pthread_cond_init(&posix_cond, pthread_condattr_default));
#else
    THROW_ERRORS(pthread_cond_init(&posix_cond, 0));
#endif
}

BCCondition::~BCCondition(void)
{
    THROW_ERRORS(pthread_cond_destroy(&posix_cond));
}

void
BCCondition::Wait(void)
{
    THROW_ERRORS(pthread_cond_wait(&posix_cond, &m_pMutex->posix_mutex));
}

BCRESULT
BCCondition::Wait(BCIntervalS *pTime)
{
    // DWORD seconds, nanoseconds;
    BCTimeS now, tResult;

    if (bc_time_now(&now) != BC_R_SUCCESS)
    {
        return (BC_R_UNEXPECTED);
    }

    if (bc_time_add(&now, pTime, &tResult) != BC_R_SUCCESS)
    {
        return (BC_R_UNEXPECTED);
    }
    // seconds = bc_time_seconds(&tResult);
    // nanoseconds = bc_time_nanoseconds(&tResult);

    return TimedWait(&tResult);
}

BCRESULT
BCCondition::TimedWait(BCTimeS *pTime)
{
	int presult;
	BCRESULT result;
	struct timespec ts;
	char strbuf[BC_STRERRORSIZE];

	ASSERT(pTime != NULL);

	/*
	 * POSIX defines a timespec's tv_sec as time_t.
	 */
	result = bc_time_secondsastimet(pTime, &ts.tv_sec);
	if (result != BC_R_SUCCESS)
		return (result);

	/*!
	 * POSIX defines a timespec's tv_nsec as long. bc_time_nanoseconds
	 * ensures its return value is < 1 billion, which will fit in a long.
	 */
	ts.tv_nsec = (long)bc_time_nanoseconds(pTime);

	do
	{
		presult = pthread_cond_timedwait(&posix_cond,
                                          &m_pMutex->posix_mutex, &ts);
		if (presult == 0)
			return (BC_R_SUCCESS);
		if (presult == ETIMEDOUT)
			return (BC_R_TIMEDOUT);
	} while (presult == EINTR);

	// bc_strerror(presult, strbuf, sizeof(strbuf));
	// LogError(_LOCAL_,
	// 		 "pthread_cond_timedwait() %s %s", "returned",
	// 		 strbuf);
	return (BC_R_UNEXPECTED);
}

int
BCCondition::TimedWait(unsigned long secs, unsigned long nanosecs)
{
    timespec rqts = { (long)secs, (long)nanosecs };

again:
    int32_t rc = ERRNO(pthread_cond_timedwait(&posix_cond,
                                          &m_pMutex->posix_mutex, &rqts));
    if (rc == 0)
        return 1;

#if (PthreadDraftVersion <= 6)
    if (rc == EAGAIN)
        return 0;
#endif

    // Some versions of unix produces this errno when the wait was
    // interrupted by a unix signal or fork.
    // Some versions of the glibc 2.0.x produces this errno when the
    // program is debugged under gdb. Straightly speaking this is non-posix
    // compliant. We catch this here to make debugging possible.
    if (rc == EINTR)
      goto again;

    if (rc == ETIMEDOUT)
        return 0;

    throw BCThreadFatal(rc);
#ifdef _MSC_VER
    return 0;
#endif
}

void
BCCondition::Signal(void)
{
    THROW_ERRORS(pthread_cond_signal(&posix_cond));
}

void
BCCondition::Broadcast(void)
{
    THROW_ERRORS(pthread_cond_broadcast(&posix_cond));
}



///////////////////////////////////////////////////////////////////////////
//
// Counting semaphore
//
///////////////////////////////////////////////////////////////////////////


BCSemaphore::BCSemaphore(uint32_t initial) : c(&m)
{
    value = initial;
}

BCSemaphore::~BCSemaphore(void)
{
}

void
BCSemaphore::Wait(void)
{
    BCMutex::Owner l(m);

    while (value == 0)
        c.Wait();

    value--;
}

int32_t
BCSemaphore::TryWait(void)
{
    BCMutex::Owner l(m);

    if (value == 0)
        return 0;

    value--;
    return 1;
}

void
BCSemaphore::Post(void)
{
	BCMutex::Owner l(m);
	value++;

    c.Signal();
}


///////////////////////////////////////////////////////////////////////////////
// BCManEvent
///////////////////////////////////////////////////////////////////////////////

BCManEvent::BCManEvent(bool bInitialState)
		: m_sLock()
		, m_cond(&m_sLock)
{
	UNUSED(bInitialState);
}

BCManEvent::~BCManEvent()
{
	//
}

void BCManEvent::Wait()
{
	if (!Wait(INFINITE))
	{
		// LogError(_LOCAL_, "Unexpected timeout on infinite wait");
		throw BCException("BCManEvent::Wait()",	"Unexpected timeout on infinite wait");
	}
}

bool BCManEvent::Wait(DWORD timeoutMillis)
{
	unsigned long secs, nanosecs;
	BCIntervalS sInterval;

	secs = timeoutMillis/1000;
	nanosecs = (timeoutMillis%1000)*1000000;
    bc_interval_set(&sInterval, secs, nanosecs);
	BCMutex::Owner lock(m_sLock);
	return (m_cond.Wait(&sInterval) == BC_R_SUCCESS) ;
}

void BCManEvent::Set()
{
	BCMutex::Owner lock(m_sLock);
	m_cond.Signal();
}


///////////////////////////////////////////////////////////////////////////
//
// Thread
//
///////////////////////////////////////////////////////////////////////////


//
// static variables
//

BCMutex* BCThread::m_pNextIdMutex;
int32_t BCThread::m_nNextId = 0;

#ifndef __rtems__
static BCThread::init_t BCThreadInit;
#else
// RTEMS calls global Ctor/Dtor in a context that is not
// a posix thread. Calls to functions to pthread_self() in
// that context returns NULL.
// So, for RTEMS we will make the thread initialization at the
// beginning of the Init task that has a posix context.
#endif

static pthread_key_t self_key;

#ifdef PthreadSupportThreadPriority
static int32_t lowest_priority;
static int32_t normal_priority;
static int32_t highest_priority;
#endif

#if defined(__osf1__) && defined(__alpha__) || defined(__VMS)
// omniORB requires a larger stack size than the default (21120) on OSF/1
static size_t stack_size = 32768;
#elif defined(__rtems__)
static size_t stack_size = ThreadStackSize;
#elif defined(__aix__)
static size_t stack_size = 262144;
#else
static size_t stack_size = 0;
#endif

//
// Initialisation function (gets called before any user code).
//

static int32_t& count() {
  static int32_t the_count = 0;
  return the_count;
}

BCThread::init_t::init_t(void)
{
    if (count()++ != 0) // only do it once however many objects get created.
        return;

    DB(cerr << "BCThread::init: posix 1003.4a/1003.1c (draft "
       << PthreadDraftVersion << ") implementation initialising\n");

#ifdef NeedPthreadInit

    pthread_init();

#endif

#if (PthreadDraftVersion == 4)
    THROW_ERRORS(pthread_keycreate(&self_key, NULL));
#else
    THROW_ERRORS(pthread_key_create(&self_key, NULL));
#endif

#ifdef PthreadSupportThreadPriority

#if defined(__osf1__) && defined(__alpha__) || defined(__VMS)

    lowest_priority = PRI_OTHER_MIN;
    highest_priority = PRI_OTHER_MAX;

#elif defined(__hpux__)

    lowest_priority = PRI_OTHER_MIN;
    highest_priority = PRI_OTHER_MAX;

#elif defined(__sunos__) && (__OSVERSION__ == 5)

    // a bug in pthread_attr_setschedparam means lowest priority is 1 not 0

    lowest_priority  = 1;
    highest_priority = 3;

#else

    lowest_priority = sched_get_priority_min(SCHED_FIFO);
    highest_priority = sched_get_priority_max(SCHED_FIFO);

#endif

    switch (highest_priority - lowest_priority) {

    case 0:
    case 1:
        normal_priority = lowest_priority;
        break;

    default:
        normal_priority = lowest_priority + 1;
        break;
    }

#endif   /* PthreadSupportThreadPriority */

    m_pNextIdMutex = new BCMutex;

    //
    // Create object for this (i.e. initial) thread.
    //

    BCThread* t = new BCThread;

    t->m_state = STATE_RUNNING;

    t->posix_thread = pthread_self ();

    DB(cerr << "initial thread " << t->id() << endl);

    THROW_ERRORS(pthread_setspecific(self_key, (void*)t));

#ifdef PthreadSupportThreadPriority

#if (PthreadDraftVersion == 4)

    THROW_ERRORS(pthread_setprio(t->posix_thread,
                                 posix_priority(PRIORITY_NORMAL)));

#elif (PthreadDraftVersion == 6)

    pthread_attr_t attr;
    pthread_attr_init(&attr);

    THROW_ERRORS(pthread_attr_setprio(&attr, posix_priority(PRIORITY_NORMAL)));

    THROW_ERRORS(pthread_setschedattr(t->posix_thread, attr));

#else

    struct sched_param sparam;

    sparam.sched_priority = posix_priority(PRIORITY_NORMAL);

    //THROW_ERRORS(pthread_setschedparam(t->posix_thread, SCHED_OTHER, &sparam));
    {
    	int retval = pthread_setschedparam(t->posix_thread, SCHED_OTHER, &sparam);
    	if (retval != 0)
    	{
    		BCRESULT result;

    		result = bc_errno2result(retval);
			//throw BCThreadFatal(retval);
    	}
    }

#endif   /* PthreadDraftVersion */

#endif   /* PthreadSupportThreadPriority */
}

BCThread::init_t::~init_t(void)
{
    if (--count() != 0) return;

    BCThread* self = BCThread::Self();
    if (!self) return;

    pthread_setspecific(self_key, 0);
    delete self;

    delete m_pNextIdMutex;
}

//
// Wrapper for thread creation.
//

void* BCThreadWrapper(LPVOID ptr)
{
    BCThread* me = (BCThread*)ptr;

    DB(cerr << "BCThread_wrapper: thread " << me->id()
       << " started\n");

    THROW_ERRORS(pthread_setspecific(self_key, me));
    if (!me->m_strName.empty())
    {
#if defined(OS_MAC) || defined(OS_IOS) // OS_MAC
    pthread_setname_np(me->m_strName.c_str());
#else // !OS_MAC
    pthread_setname_np(pthread_self(),  me->m_strName.c_str());
#endif // OS_MAC
    }
	me->m_startSignal.Post();

    //
    // Now invoke the thread function with the given argument.
    //

    if (me->fn_void != NULL) {
        (*me->fn_void)(me->m_pThreadArg);
        BCThread::Exit();
    }

    if (me->fn_ret != NULL) {
        void* return_value = (*me->fn_ret)(me->m_pThreadArg);
        BCThread::Exit(return_value);
    }

    if (me->m_bDetached) {
        me->Run(me->m_pThreadArg);
        BCThread::Exit();
    } else {
        void* return_value = me->RunUndetached(me->m_pThreadArg);
        BCThread::Exit(return_value);
    }

    // should never get here.

    return 0;
}


//
// Constructors for omni_thread - set up the thread object but don't
// start it running.
//

// construct a detached thread running a given function.

BCThread::BCThread(
    void (*fn)(void*), 
    void* arg, 
    priority_t pri,
    LPCSTR lpszThreadName)
    : m_startSignal(0)
    , m_strName(lpszThreadName)
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
    : m_startSignal(0)
    , m_strName(lpszThreadName)
{
    CommonConstructor(arg, pri, 0);
    fn_void = NULL;
    fn_ret = fn;
}

// construct a thread which will run either run() or run_undetached().

BCThread::BCThread(void* arg, priority_t pri)
    : m_startSignal(0)
{
    CommonConstructor(arg, pri, 1);
    fn_void = NULL;
    fn_ret = NULL;
}

// common part of all constructors.

void
BCThread::CommonConstructor(void* arg, priority_t pri, int32_t det)
{
    m_state = STATE_NEW;
    m_priority = pri;

    m_pNextIdMutex->Lock();
    m_nId = m_nNextId++;
    m_pNextIdMutex->Unlock();

    m_pThreadArg = arg;
    m_bDetached = det;     // may be altered in start_undetached()

    _dummy       = 0;
    _values      = 0;
    _value_alloc = 0;
    // posix_thread is set up in initialisation routine or start().
}


//
// Destructor for omni_thread.
//

BCThread::~BCThread(void)
{
    DB(cerr << "destructor called for thread " << id() << endl);
    if (_values) {
        for (key_t i=0; i < _value_alloc; i++) {
            if (_values[i]) {
                delete _values[i];
            }
        }
        delete [] _values;
    }
}


//
// Start the thread
//

void
BCThread::Start(void)
{
	BCMutex::Owner Lock(m_Mutex);

    if (m_state != STATE_NEW)
        throw BCThreadInvalid();

    pthread_attr_t attr;

#if (PthreadDraftVersion == 4)
    pthread_attr_create(&attr);
#else
    pthread_attr_init(&attr);
#endif

#if (PthreadDraftVersion == 8)
    pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_UNDETACHED);
#endif

#ifdef PthreadSupportThreadPriority

#if (PthreadDraftVersion <= 6)

    THROW_ERRORS(pthread_attr_setprio(&attr, posix_priority(m_priority)));

#else

    struct sched_param sparam;

    sparam.sched_priority = posix_priority(m_priority);

    //THROW_ERRORS(pthread_attr_setschedparam(&attr, &sparam));
    {
    	int retval = pthread_attr_setschedparam(&attr, &sparam);
    	if (retval != 0)
    	{
    		BCRESULT result;

    		result = bc_errno2result(retval);
			//throw BCThreadFatal(retval);
    	}
    }

#endif  /* PthreadDraftVersion */

#endif  /* PthreadSupportThreadPriority */

#if !defined(__linux__)
    if (stack_size) {
      THROW_ERRORS(pthread_attr_setstacksize(&attr, stack_size));
    }
#endif


#if (PthreadDraftVersion == 4)
    THROW_ERRORS(pthread_create(&posix_thread, attr,BCThreadWrapper,
                                (void*)this));
    pthread_attr_delete(&attr);
#else
    THROW_ERRORS(pthread_create(&posix_thread, &attr, BCThreadWrapper,
                                (void*)this));
    pthread_attr_destroy(&attr);
#endif

    m_state = STATE_RUNNING;

    if (m_bDetached) {

#if (PthreadDraftVersion <= 6)
//此处存疑，原来的版本传参为&posix_thread，我改成了posix_thread
        THROW_ERRORS(pthread_detach(posix_thread));
#else
        THROW_ERRORS(pthread_detach(posix_thread));
#endif
    }
	m_startSignal.Wait();
}


//
// Start a thread which will run the member function run_undetached().
//

void
BCThread::StartUndetached(void)
{
    if ((fn_void != NULL) || (fn_ret != NULL))
        throw BCThreadInvalid();

    m_bDetached = 0;
    Start();
}


//
// join - simply check error conditions & call pthread_join.
//

void
BCThread::Join(void** status)
{
    m_Mutex.Lock();

    if ((m_state != STATE_RUNNING) && (m_state != STATE_TERMINATED)) {
        m_Mutex.Unlock();
        throw BCThreadInvalid();
    }

    m_Mutex.Unlock();

    if (this == Self())
        throw BCThreadInvalid();

    if (m_bDetached)
        throw BCThreadInvalid();

    DB(cerr << "BCThread::join: doing pthread_join\n");

    THROW_ERRORS(pthread_join(posix_thread, status));

    DB(cerr << "BCThread::join: pthread_join succeeded\n");

#if (PthreadDraftVersion == 4)
    // With draft 4 pthreads implementations (HPUX 10.x and
    // Digital Unix 3.2), have to detach the thread after
    // join. If not, the storage for the thread will not be
    // be reclaimed.
    THROW_ERRORS(pthread_detach(&posix_thread));
#endif

    delete this;
}


//
// Change this thread's priority.
//

void
BCThread::SetPriority(priority_t pri)
{
    BCMutex::Owner Lock(m_Mutex);

    if (m_state != STATE_RUNNING)
        throw BCThreadInvalid();

    m_priority = pri;

#ifdef PthreadSupportThreadPriority

#if (PthreadDraftVersion == 4)

    THROW_ERRORS(pthread_setprio(posix_thread, posix_priority(pri)));

#elif (PthreadDraftVersion == 6)

    pthread_attr_t attr;
    pthread_attr_init(&attr);

    THROW_ERRORS(pthread_attr_setprio(&attr, posix_priority(pri)));

    THROW_ERRORS(pthread_setschedattr(posix_thread, attr));

#else

    struct sched_param sparam;

    sparam.sched_priority = posix_priority(pri);

    //THROW_ERRORS(pthread_setschedparam(posix_thread, SCHED_OTHER, &sparam));

    {
    	int retval = pthread_setschedparam(posix_thread, SCHED_OTHER, &sparam);
    	if (retval != 0)
    	{
    		BCRESULT result;

    		result = bc_errno2result(retval);
			//throw BCThreadFatal(retval);
    	}
    }

#endif   /* PthreadDraftVersion */

#endif   /* PthreadSupportThreadPriority */
}


//
// create - construct a new thread object and start it running.  Returns thread
// object if successful, null pointer if not.
//

// detached version

BCThread*
BCThread::Create(void (*fn)(void*), void* arg, priority_t pri)
{
    BCThread* t = new BCThread(fn, arg, pri);

    t->Start();

    return t;
}

// undetached version

BCThread*
BCThread::Create(void* (*fn)(void*), void* arg, priority_t pri)
{
    BCThread* t = new BCThread(fn, arg, pri);

    t->Start();

    return t;
}


//
// exit() _must_ lock the mutex even in the case of a detached thread.  This is
// because a thread may run to completion before the thread that created it has
// had a chance to get out of start().  By locking the mutex we ensure that the
// creating thread must have reached the end of start() before we delete the
// thread object.  Of course, once the call to start() returns, the user can
// still incorrectly refer to the thread object, but that's their problem.
//

void
BCThread::Exit(void* return_value)
{
    BCThread* me = Self();

    if (me)
      {
        me->m_Mutex.Lock();

        me->m_state = STATE_TERMINATED;

        me->m_Mutex.Unlock();

        DB(cerr << "BCThread::Exit: thread " << me->m_nId() << " detached "
           << me->m_bDetached << " return value " << return_value << endl);

        if (me->m_bDetached)
          delete me;
      }
    else
      {
        DB(cerr << "BCThread::Exit: called with a non-BCThread. Exit quietly." << endl);
      }

    pthread_exit(return_value);
}


BCThread*
BCThread::Self(void)
{
    BCThread* me;

#if (/*PthreadDraftVersion <= 6*/false)

    THROW_ERRORS(pthread_getspecific(self_key, (void**)&me));

#else

    me = (BCThread *)pthread_getspecific(self_key);

#endif

    if (!me) {
      // This thread is not created by omni_thread::start because it
      // doesn't has a class omni_thread instance attached to its key.
      DB(cerr << "BCThread::Self: called with a non-BCThread. NULL is returned." << endl);
    }

    return me;
}


void
BCThread::Yield(void)
{
#ifdef __ANDROID__
    THROW_ERRORS(sched_yield());
#else // !__ANDROID__
#if (PthreadDraftVersion == 6)
    pthread_yield(NULL);
#elif (PthreadDraftVersion < 9)
#if defined(OS_MAC) || defined(OS_IOS) // OS_MAC || OS_IOS
    pthread_yield_np();
#else // !OS_MAC && !OS_IOS
    pthread_yield();
#endif // OS_MAC
#else
    THROW_ERRORS(sched_yield());
#endif
#endif // __ANDROID__
}


void
BCThread::Sleep(unsigned long secs, unsigned long nanosecs)
{
    timespec rqts = { (long)secs, (long)nanosecs};

#ifndef NoNanoSleep

    timespec remain;
    while (nanosleep(&rqts, &remain)) {
      if (errno == EINTR) {
        rqts.tv_sec  = remain.tv_sec;
        rqts.tv_nsec = remain.tv_nsec;
        continue;
      }
      else
        throw BCThreadFatal(errno);
    }
#else // NONanoSleep

#if defined(__osf1__) && defined(__alpha__) || defined(__hpux__) && (__OSVERSION__ == 10) || defined(__VMS) || defined(__SINIX__) || defined (__POSIX_NT__)

    if (pthread_delay_np(&rqts) != 0)
        throw BCThreadFatal(errno);

#elif defined(__linux__) || defined(__aix__)

    if (secs > 2000) {
      while ((secs = ::sleep(secs))) ;
    } else {
        usleep(secs * 1000000 + (nanosecs / 1000));
    }

#elif defined(__darwin__) || defined(__macos__)

	UNUSED(rqts);
    // Single UNIX Specification says argument of usleep() must be
    // less than 1,000,000.
    secs += nanosecs / 1000000000;
    nanosecs %= 1000000000;
    while ((secs = ::sleep(secs))) ;
    usleep(nanosecs / 1000);

#else

	UNUSED(rqts);
    throw BCThreadInvalid();

#endif

	UNUSED(rqts);
#endif  /* NoNanoSleep */
}


void
BCThread::GetTime(unsigned long* abs_sec, unsigned long* abs_nsec,
                      unsigned long rel_sec, unsigned long rel_nsec)
{
    timespec abs;

#if defined(__osf1__) && defined(__alpha__) ||\
	defined(__hpux__) && (__OSVERSION__ == 10) ||\
	 defined(__VMS) || defined(__SINIX__) || defined(__POSIX_NT__)

    timespec rel;
    rel.tv_sec = rel_sec;
    rel.tv_nsec = rel_nsec;
    THROW_ERRORS(pthread_get_expiration_np(&rel, &abs));

#else

#ifdef HAVE_CLOCK_GETTIME       /* __linux__ || __aix__ */

    clock_gettime(CLOCK_REALTIME, &abs);

#elif defined(HAVE_GETTIMEOFDAY)        /* defined(__linux__) || defined(__aix__) || defined(__SCO_VERSION__) || defined(__darwin__) || defined(__macos__) */

    struct timeval tv;
    gettimeofday(&tv, NULL);
    abs.tv_sec = tv.tv_sec;
    abs.tv_nsec = tv.tv_usec * 1000;

#else
#error no get time support
#endif  /* __linux__ || __aix__ */

    abs.tv_nsec += rel_nsec;
    abs.tv_sec += rel_sec + abs.tv_nsec / 1000000000;
    abs.tv_nsec = abs.tv_nsec % 1000000000;

#endif  /* __osf1__ && __alpha__ */

    *abs_sec = abs.tv_sec;
    *abs_nsec = abs.tv_nsec;
}


int
BCThread::posix_priority(priority_t pri)
{
#ifdef PthreadSupportThreadPriority
    switch (pri) {

    case PRIORITY_LOW:
        return lowest_priority;

    case PRIORITY_NORMAL:
        return normal_priority;

    case PRIORITY_HIGH:
        return highest_priority;

    }
#else
	UNUSED(pri);
#endif

    throw BCThreadInvalid();
#ifdef _MSC_VER
    return 0;
#endif
}

void
BCThread::StackSize(unsigned long sz)
{
  stack_size = sz;
}

unsigned long
BCThread::StackSize()
{
  return stack_size;
}

//
// Dummy thread
//
//*********************************************************************************************************************
class BCThreadDummy : public BCThread {
public:
  inline BCThreadDummy() : BCThread()
  {
    _dummy = 1;
    m_state = STATE_RUNNING;
    posix_thread = pthread_self();
    THROW_ERRORS(pthread_setspecific(self_key, (void*)this));
  }
  inline ~BCThreadDummy()
  {
    THROW_ERRORS(pthread_setspecific(self_key, 0));
  }
};
//**********************************************************************************************************************

BCThread*
BCThread::create_dummy()
{
  if (BCThread::Self())
    throw BCThreadInvalid();

  return new BCThreadDummy;
}

void
BCThread::release_dummy()
{
  BCThread* self = BCThread::Self();
  if (!self || !self->_dummy)
    throw BCThreadInvalid();

  BCThreadDummy* dummy = (BCThreadDummy*)self;
  delete dummy;
}

//threaddata.cc
static BCThread::key_t allocated_keys = 0;

BCThread::key_t
BCThread::allocate_key()
{
  BCMutex::Owner  Lock(*m_pNextIdMutex);
  return ++allocated_keys;
}

BCThread::value_t*
BCThread::set_value(key_t k, value_t* v)
{
  if (k == 0) return 0;
  if (k > _value_alloc) {
    m_pNextIdMutex->Lock();
    key_t alloc = allocated_keys;
    m_pNextIdMutex->Unlock();

    if (k > alloc) return 0;

    value_t** nv = new value_t*[alloc];
    key_t i = 0;
    if (_values) {
      for (; i < _value_alloc; i++)
        nv[i] = _values[i];
      delete [] _values;
    }
    for (; i < alloc; i++)
      nv[i] = 0;

    _values = nv;
    _value_alloc = alloc;
  }
  if (_values[k-1]) delete _values[k-1];
  _values[k-1] = v;
  return v;
}

BCThread::value_t*
BCThread::get_value(key_t k)
{
  if (k > _value_alloc) return 0;
  return _values[k-1];
}

BCThread::value_t*
BCThread::remove_value(key_t k)
{
  if (k > _value_alloc) return 0;
  value_t* v = _values[k-1];
  _values[k-1] = 0;
  return v;
}
}
