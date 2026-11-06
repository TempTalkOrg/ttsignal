
#ifndef BC_BASE_SPINNING_MUTEX_H_
#define BC_BASE_SPINNING_MUTEX_H_

#include <algorithm>
#include <atomic>

#include "BC/Config.h"
#include "BC/base/yield_processor.h"
#include "BC/base/base_export.h"
#include "BC/base/compiler_specific.h"
#include "BC/build/build_config.h"

#if defined(OS_WIN)
// Needed for function prototypes.
#include <concurrencysal.h>
#include <sal.h>
#include <specstrings.h>

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus
    
// typedef and define the most commonly used Windows integer types.

typedef unsigned long DWORD;
typedef long LONG;
typedef __int64 LONGLONG;
typedef unsigned __int64 ULONGLONG;

#define VOID void
typedef char CHAR;
typedef short SHORT;
typedef long LONG;
typedef int INT;
typedef unsigned int UINT;
typedef unsigned int* PUINT;
typedef unsigned __int64 UINT64;
typedef void* LPVOID;
typedef void* PVOID;
typedef void* HANDLE;
typedef int BOOL;
typedef unsigned char BYTE;
typedef BYTE BOOLEAN;
typedef DWORD ULONG;
typedef unsigned short WORD;
typedef WORD UWORD;
typedef WORD ATOM;


typedef struct _RTL_SRWLOCK RTL_SRWLOCK;
typedef RTL_SRWLOCK SRWLOCK, * PSRWLOCK;

struct CHROME_SRWLOCK {
    PVOID Ptr;
};
// The trailing white-spaces after this macro are required, for compatibility
// with the definition in winnt.h.
#define RTL_SRWLOCK_INIT {0}                            // NOLINT
#define SRWLOCK_INIT RTL_SRWLOCK_INIT

// clang-format on

// Define some macros needed when prototyping Windows functions.
#ifndef DECLSPEC_IMPORT
#define DECLSPEC_IMPORT __declspec(dllimport)
#endif // DECLSPEC_IMPORT
#define WINBASEAPI DECLSPEC_IMPORT
#define WINUSERAPI DECLSPEC_IMPORT
#define WINAPI __stdcall
#define APIENTRY WINAPI
#define CALLBACK __stdcall

// Needed for LockImpl.
WINBASEAPI _Releases_exclusive_lock_(*SRWLock) VOID WINAPI
ReleaseSRWLockExclusive(_Inout_ PSRWLOCK SRWLock);
WINBASEAPI BOOLEAN WINAPI TryAcquireSRWLockExclusive(_Inout_ PSRWLOCK SRWLock);
WINBASEAPI _Acquires_exclusive_lock_(*SRWLock) VOID WINAPI
AcquireSRWLockExclusive(_Inout_ PSRWLOCK SRWLock);

#ifdef __cplusplus
}
#endif // __cplusplus
#endif // OS_WIN

// POSIX is not only UNIX, e.g. macOS and other OSes. We do use Linux-specific
// features such as futex(2).
#if defined(OS_LINUX) || defined(OS_CHROMEOS) || defined(OS_ANDROID)
#define PA_HAS_LINUX_KERNEL
#endif

// On some platforms, we implement locking by spinning in userspace, then going
// into the kernel only if there is contention. This requires platform support,
// namely:
// - On Linux, futex(2)
// - On Windows, a fast userspace "try" operation which is available
//   with SRWLock
// - Otherwise, a fast userspace pthread_mutex_trylock().
//
// On macOS, pthread_mutex_trylock() is fast by default starting with macOS
// 10.14. Chromium targets an earlier version, so it cannot be known at
// compile-time. So we use something different. On other POSIX systems, we
// assume that pthread_mutex_trylock() is suitable.
//
// Otherwise, a userspace spinlock implementation is used.
#if defined(PA_HAS_LINUX_KERNEL) || defined(OS_WIN) || \
    (defined(OS_POSIX) && !defined(OS_APPLE)) || defined(OS_FUCHSIA)
#define PA_HAS_FAST_MUTEX
#endif

#if defined(OS_POSIX)
#include <errno.h>
#include <pthread.h>
#endif

#if defined(OS_APPLE)

#include <os/lock.h>

// os_unfair_lock is available starting with OS X 10.12, and Chromium targets
// 10.11 at the minimum, so the symbols are not always available *at runtime*.
// But we build with a 11.x SDK, so it's always in the headers.
//
// However, since the majority of clients have at least 10.12 (released late
// 2016), we declare the symbols here, marking them weak. They will be nullptr
// on 10.11, and defined on more recent versions.

// Silence the compiler warning, here and below.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunguarded-availability"

#define PA_WEAK __attribute__((weak))

extern "C" {

PA_WEAK void os_unfair_lock_lock(os_unfair_lock_t lock);
PA_WEAK bool os_unfair_lock_trylock(os_unfair_lock_t lock);
PA_WEAK void os_unfair_lock_unlock(os_unfair_lock_t lock);
}

#pragma clang diagnostic pop

#endif  // defined(OS_APPLE)

#if defined(OS_FUCHSIA)
#include <lib/sync/mutex.h>
#endif

namespace Base {

// The behavior of this class depends on whether PA_HAS_FAST_MUTEX is defined.
// 1. When it is defined:
//
// Simple spinning lock. It will spin in user space a set number of times before
// going into the kernel to sleep.
//
// This is intended to give "the best of both worlds" between a SpinLock and
// base::Lock:
// - SpinLock: Inlined fast path, no external function calls, just
//   compare-and-swap. Short waits do not go into the kernel. Good behavior in
//   low contention cases.
// - base::Lock: Good behavior in case of contention.
//
// We don't rely on base::Lock which we could make spin (by calling Try() in a
// loop), as performance is below a custom spinlock as seen on high-level
// benchmarks. Instead this implements a simple non-recursive mutex on top of
// the futex() syscall on Linux, and SRWLock on Windows. The main difference
// between this and a libc implementation is that it only supports the simplest
// path: private (to a process), non-recursive mutexes with no priority
// inheritance, no timed waits.
//
// As an interesting side-effect to be used in the allocator, this code does not
// make any allocations, locks are small with a constexpr constructor and no
// destructor.
//
// 2. Otherwise: This is a simple SpinLock, in the sense that it does not have
// any awareness of other threads' behavior. One exception: x86 macOS uses
// os_unfair_lock() if available, which is the case for macOS >= 10.12, that is
// most clients.
class /*LOCKABLE*/ BASE_EXPORT SpinningMutex {
public:
    class BASE_EXPORT Owner
    {
        SpinningMutex& m_mutex;
    public:
        Owner(SpinningMutex& m) : m_mutex(m)
        {
            m_mutex.Acquire();
        }
        ~Owner(void)
        {
            m_mutex.Release();
        }
    private:
        // dummy copy constructor and operator= to prevent copying
        DECLARE_NO_COPY_CLASS(Owner)
    };
 public:
  inline constexpr SpinningMutex();
  ALWAYS_INLINE void Acquire() /*EXCLUSIVE_LOCK_FUNCTION()*/;
  ALWAYS_INLINE void Release() /*UNLOCK_FUNCTION()*/;
  ALWAYS_INLINE void Lock() { Acquire(); }
  ALWAYS_INLINE void Unlock() { Release(); }
  ALWAYS_INLINE bool Try() /*EXCLUSIVE_TRYLOCK_FUNCTION(true)*/;
  void AssertAcquired() const {}  // Not supported.
  void Reinit() /*UNLOCK_FUNCTION()*/;

 private:
  void LockSlow() /*EXCLUSIVE_LOCK_FUNCTION()*/;

  // Same as SpinLock, not scientifically calibrated. Consider lowering later,
  // as the slow path has better characteristics than SpinLocks's.
  static constexpr int kSpinCount = 1000;

#if defined(PA_HAS_FAST_MUTEX)

#if defined(PA_HAS_LINUX_KERNEL)
  void FutexWait();
  void FutexWake();

  static constexpr int kUnlocked = 0;
  static constexpr int kLockedUncontended = 1;
  static constexpr int kLockedContended = 2;

  std::atomic<int32_t> state_{kUnlocked};
#elif defined(OS_WIN)
  CHROME_SRWLOCK lock_ = SRWLOCK_INIT;
#elif defined(OS_POSIX)
  pthread_mutex_t lock_ = PTHREAD_MUTEX_INITIALIZER;
#elif defined(OS_FUCHSIA)
  sync_mutex lock_;
#endif

#else  // defined(PA_HAS_FAST_MUTEX)
  std::atomic<bool> lock_{false};

#if defined(OS_APPLE) && !defined(PA_NO_OS_UNFAIR_LOCK_CRBUG_1267256)

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunguarded-availability"
  os_unfair_lock unfair_lock_ = OS_UNFAIR_LOCK_INIT;
#pragma clang diagnostic pop

#endif  // defined(OS_APPLE) && !defined(PA_NO_OS_UNFAIR_LOCK_CRBUG_1267256)

  // Spinlock-like, fallback.
  ALWAYS_INLINE bool TrySpinLock();
  ALWAYS_INLINE void ReleaseSpinLock();
  void LockSlowSpinLock();

#endif
};

ALWAYS_INLINE void SpinningMutex::Acquire() {
  int tries = 0;
  int backoff = 1;
  // Busy-waiting is inlined, which is fine as long as we have few callers. This
  // is only used for the partition lock, so this is the case.
  do {
    if (LIKELY(Try()))
      return;
    // Note: Per the intel optimization manual
    // (https://software.intel.com/content/dam/develop/public/us/en/documents/64-ia-32-architectures-optimization-manual.pdf),
    // the "pause" instruction is more costly on Skylake Client than on previous
    // (and subsequent?) architectures. The latency is found to be 141 cycles
    // there. This is not a big issue here as we don't spin long enough for this
    // to become a problem, as we spend a maximum of ~141k cycles ~= 47us at
    // 3GHz in "pause".
    //
    // Also, loop several times here, following the guidelines in section 2.3.4
    // of the manual, "Pause latency in Skylake Client Microarchitecture".
    for (int yields = 0; yields < backoff; yields++) {
      YIELD_PROCESSOR;
      tries++;
    }
    constexpr int kMaxBackoff = 64;
    backoff = BCMIN(kMaxBackoff, backoff << 1);
  } while (tries < kSpinCount);

  LockSlow();
}

inline constexpr SpinningMutex::SpinningMutex() = default;

#if defined(PA_HAS_FAST_MUTEX)

#if defined(PA_HAS_LINUX_KERNEL)

ALWAYS_INLINE bool SpinningMutex::Try() {
  // Using the weak variant of compare_exchange(), which may fail spuriously. On
  // some architectures such as ARM, CAS is typically performed as a LDREX/STREX
  // pair, where the store may fail. In the strong version, there is a loop
  // inserted by the compiler to retry in these cases.
  //
  // Since we are retrying in Lock() anyway, there is no point having two nested
  // loops.
  int expected = kUnlocked;
  return (state_.load(std::memory_order_relaxed) == expected) &&
         state_.compare_exchange_weak(expected, kLockedUncontended,
                                      std::memory_order_acquire,
                                      std::memory_order_relaxed);
}

ALWAYS_INLINE void SpinningMutex::Release() {
  if (UNLIKELY(state_.exchange(kUnlocked, std::memory_order_release) ==
               kLockedContended)) {
    // |kLockedContended|: there is a waiter to wake up.
    //
    // Here there is a window where the lock is unlocked, since we just set it
    // to |kUnlocked| above. Meaning that another thread can grab the lock
    // in-between now and |FutexWake()| waking up a waiter. Aside from
    // potentially fairness, this is not an issue, as the newly-awaken thread
    // will check that the lock is still free.
    //
    // There is a small pessimization here though: if we have a single waiter,
    // then when it wakes up, the lock will be set to |kLockedContended|, so
    // when this waiter releases the lock, it will needlessly call
    // |FutexWake()|, even though there are no waiters. This is supported by the
    // kernel, and is what bionic (Android's libc) also does.
    FutexWake();
  }
}

#elif defined(OS_WIN)

ALWAYS_INLINE bool SpinningMutex::Try() {
  return !!::TryAcquireSRWLockExclusive(reinterpret_cast<PSRWLOCK>(&lock_));
}

ALWAYS_INLINE void SpinningMutex::Release() {
  ::ReleaseSRWLockExclusive(reinterpret_cast<PSRWLOCK>(&lock_));
}

#elif defined(OS_POSIX)

ALWAYS_INLINE bool SpinningMutex::Try() {
  int retval = pthread_mutex_trylock(&lock_);
  PA_DCHECK(retval == 0 || retval == EBUSY);
  return retval == 0;
}

ALWAYS_INLINE void SpinningMutex::Release() {
  int retval = pthread_mutex_unlock(&lock_);
  PA_DCHECK(retval == 0);
}

#elif defined(OS_FUCHSIA)

ALWAYS_INLINE bool SpinningMutex::Try() {
  return sync_mutex_trylock(&lock_) == ZX_OK;
}

ALWAYS_INLINE void SpinningMutex::Release() {
  sync_mutex_unlock(&lock_);
}

#endif

#else  // defined(PA_HAS_FAST_MUTEX)

ALWAYS_INLINE bool SpinningMutex::TrySpinLock() {
  // Possibly faster than CAS. The theory is that if the cacheline is shared,
  // then it can stay shared, for the contended case.
  return !lock_.load(std::memory_order_relaxed) &&
         !lock_.exchange(true, std::memory_order_acquire);
}

ALWAYS_INLINE void SpinningMutex::ReleaseSpinLock() {
  lock_.store(false, std::memory_order_release);
}

#if defined(OS_APPLE)

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunguarded-availability"

ALWAYS_INLINE bool SpinningMutex::Try() {
  // ARM64 macOS is macOS 11.x at least, guaranteed to have os_unfair_lock().
#if defined(OS_MAC) && defined(ARCH_CPU_ARM64)
  return os_unfair_lock_trylock(&unfair_lock_);
#else
  if (LIKELY(os_unfair_lock_trylock))
    return os_unfair_lock_trylock(&unfair_lock_);

  return TrySpinLock();
#endif  // defined(OS_MAC) && defined(ARCH_CPU_ARM64)
}

ALWAYS_INLINE void SpinningMutex::Release() {
#if defined(OS_MAC) && defined(ARCH_CPU_ARM64)
  return os_unfair_lock_unlock(&unfair_lock_);
#else
  // Always testing trylock(), since the definitions are all or nothing.
  if (LIKELY(os_unfair_lock_trylock))
    return os_unfair_lock_unlock(&unfair_lock_);

  return ReleaseSpinLock();
#endif  // defined(OS_MAC) && defined(ARCH_CPU_ARM64)
}

ALWAYS_INLINE void SpinningMutex::LockSlow() {
#if defined(OS_MAC) && defined(ARCH_CPU_ARM64)
  return os_unfair_lock_lock(&unfair_lock_);
#else
  if (LIKELY(os_unfair_lock_trylock))
    return os_unfair_lock_lock(&unfair_lock_);

  return LockSlowSpinLock();
#endif  // defined(OS_MAC) && defined(ARCH_CPU_ARM64)
}

#pragma clang diagnostic pop

#else
ALWAYS_INLINE bool SpinningMutex::Try() {
  return TrySpinLock();
}

ALWAYS_INLINE void SpinningMutex::Release() {
  return ReleaseSpinLock();
}

ALWAYS_INLINE void SpinningMutex::LockSlow() {
  return LockSlowSpinLock();
}

#endif  // defined(OS_APPLE)

#endif  // defined(PA_HAS_FAST_MUTEX)

}  // namespace Base

#endif  // BC_BASE_SPINNING_MUTEX_H_
