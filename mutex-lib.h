/******************************************************************************
**
** Copyright (C) 2019 by bsdlicexamples.com
** 
** Permission to use, copy, modify, and/or distribute this software for 
** any purpose with or without fee is hereby granted.
** 
** THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES 
** WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF 
** MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY 
** SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES 
** WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN 
** ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR 
** IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
**
******************************************************************************/
/******************************************************************************
** LATEST REVISION/AUTHOR
** December-07-2019/Andrey Tsigler
******************************************************************************/
#ifndef MUTEX_LIB_H_INCLUDED
#define MUTEX_LIB_H_INCLUDED
#include <linux/futex.h>
#include <stdatomic.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sched.h>

/******************************************************************************
** OVERVIEW:
**
** This file contains APIs to manage the mutual exclusion locks for
** processes and threads very efficiently. All APIs in the mutex library,
** including the bsdlicMutexInit(), bsdlicMutexLock(), bsdlicMutexUnlock() and 
** bsdlicMutexNoYieldUnlock(), are defined as macros. In order to use 
** the APIs, simply include the mutex-lib.h file into your application and 
** make sure that the application is compiled with optimization flags, 
** such as -O2. The optimization is needed in order to achieve good 
** performance for the lock/unlock functions.
** 
** In a multi-process environment the same functionality can be achieved
** using the sem_wait()/sem_post() functions. The bsdlicMutex APIs are faster
** than sem_wait()/sem_post() in all use cases. 
** 
** In the multi-threaded environment the same functionality can be achieved
** using the pthread_mutex_lock()/pthread_mutex_unlock() functions.
** The bsdlicMutexLock()/bsdlicMutexUnlock() APIs are faster in all use cases.
**
** The bsdlicMutex functions are much simpler than the pthread_mutex 
** functions. For example there is no support for recursive mutexes or 
** timed lock functionality. 
**
** The bsdlicMutex does support static initializer for non-shared mutexes 
** using the BSDLIC_MUTEX_INIT_PRIVATE macro. There is no analogous macro
** for initializing shared mutexes, because shared memory needs to be created 
** dynamically for the shared mutexes.
** 
** PERFORMANCE CONSIDERATIONS:
**
** The standard functions pthread_mutex_lock()/pthread_mutex_unlock() are 
** already very fast. For example on a 1.66GHZ dual core x86 in 64-bit mode
** the pthread_mutex_lock()/unlock() combo adds 571ms (milliseconds) over
** 10 million iterations when there is no contention for the mutex.
** This means that the application needs to perform millions of lock/unlock
** operations per second in order to see any measurable impact from the 
** mutex locking APIs.
**
** The bsdlicMutexLock()/bsdlicMutexUnlock() functions add 408ms over 
** 10 million iterations, which in some use cases can be a noticeable
** improvement over the standard pthread_mutex functions.
**
** When multiple threads are heavily contending for the mutex, the 
** bsdlicMutex functions are significantly faster than the 
** pthread_mutex functions. The key reason for this performance advantage
** is that the bsdlicMutexUnlock() function detects when there is a lot of 
** contention for the mutex and yields the cpu using the sched_yield() 
** function. The sched_yield() function is primarily intended for tasks 
** scheduled using SCHED_RR or SCHED_FIFO policy, but also works 
** for non-real time tasks scheduled with SCHED_OTHER policy. 
** Yielding the CPU enables the bsdlicMutexUnlock() function to reduce the 
** number of unnecessary calls to the futex() system call, so that the
** majority of the semaphore lock/unlock operations are performed in user 
** space using atomic functions.
**
** The test with two heavily contending threads that perform 10 million 
** lock/unlock operations around a small critical section completes in 
** 3213ms with pthread_mutex functions and 1804ms with bsdlicMutex.
**
** The same test with three threads is 4067ms with pthread_mutex and 
** 2460ms with bsdlicMutex. 
**
** With 10 threads contending for the same mutex the pthread_mutex 
** test completes in 17356ms and the bsdlicMutex test completes in 7263ms.
**
** For use cases where yielding the CPU is not desirable, there is an 
** alternate macro bsdlicMutexNoYieldUnlock(), which unlocks the mutex
** without calling sched_yield(). This macro is smaller and just as fast
** as the bsdlicMutexUnlock() when there is little contention for the lock.
**
******************************************************************************/


/* The mutex is currently owned by a thread or a process.
*/
#define BSDLIC_MUTEX_OWNED_FLAG 0x80000000

/* The number of tasks waiting on the mutex.
*/
#define BSDLIC_MUTEX_WAIT_COUNT_MASK 0x7FFFFFFF

#define bsdlic_futex(a,b,c,d,e,f) syscall (SYS_futex, a, b, c, d, e, f)

typedef struct 
{
  _Atomic unsigned int val;
  unsigned int shared;
} bsdlicMutex_t;

/* Static initializer for the private mutex.
*/
#define BSDLIC_MUTEX_INIT_PRIVATE {0,0}

/******************************************************************************
** Initialize the mutex.
** This function doesn't allocate any memory, so there is no corresponding
** function to delete the mutex.
**
**   mutex - (input) Pointer to the mutex lock.
**   shared_flag - (input) 0-Thread-only. 1-Thread or Process.
**
** Return Values:
**   None
******************************************************************************/
#define bsdlicMutexInit(mutex, shared_flag)   \
({                                                    \
  (mutex)->val = 0;                                   \
  (mutex)->shared = (shared_flag);                    \
})

/******************************************************************************
** Unlock the mutex.
** The mutex must be locked when calling this function.
** If this function is called for an unlocked mutex then the mutex state
** becomes corrupted. 
**
**   mutex - (input) Pointer to the mutex lock.
**
** Return Values:
**   None
******************************************************************************/
#define bsdlicMutexUnlock(mutex) \
({                                                                          \
  unsigned int mutex_val;                                                   \
                                                                            \
  mutex_val = BSDLIC_MUTEX_WAIT_COUNT_MASK & atomic_fetch_sub(&(mutex)->val,\
                              (BSDLIC_MUTEX_OWNED_FLAG | 1));               \
  if (mutex_val > 1)                                                        \
  {                                                                         \
    if (mutex_val > 2)                                                      \
    {                                                                       \
      sched_yield();                                                        \
      mutex_val = atomic_load(&(mutex)->val);                               \
    } else                                                                  \
    {                                                                       \
      mutex_val = 0;                                                        \
    }                                                                       \
    if (0 == (mutex_val & BSDLIC_MUTEX_OWNED_FLAG))                         \
    {                                                                       \
      if (0 == bsdlic_futex (&(mutex)->val,                                 \
                    ((mutex)->shared)?FUTEX_WAKE:FUTEX_WAKE_PRIVATE,        \
                    1, 0, 0, 0))                                            \
      {                                                                     \
        sched_yield();                                                      \
      }                                                                     \
    }                                                                       \
  }                                                                         \
})

/******************************************************************************
** Unlock the mutex without yielding the CPU.
** The mutex must be locked when calling this function.
** If this function is called for an unlocked mutex then the mutex state
** becomes corrupted. 
**
**   mutex - (input) Pointer to the mutex lock.
**
** Return Values:
**   None
******************************************************************************/
#define bsdlicMutexNoYieldUnlock(mutex) \
({                                                                           \
  if (1 < (BSDLIC_MUTEX_WAIT_COUNT_MASK &                                    \
             atomic_fetch_sub(&(mutex)->val, (BSDLIC_MUTEX_OWNED_FLAG | 1))))\
  {                                                                          \
    (void) bsdlic_futex (&(mutex)->val,                                      \
                    ((mutex)->shared)?FUTEX_WAKE:FUTEX_WAKE_PRIVATE,         \
                    1, 0, 0, 0);                                             \
  }                                                                          \
})

/******************************************************************************
** Lock the mutex.
**
** If another thread or process already owns the mutex 
** then the caller blocks until the mutex is unlocked. 
**
**   mutex - (input) Pointer to the mutex lock.
**
** Return Values:
**   None
******************************************************************************/
#define bsdlicMutexLock(mutex)  \
({                                                                           \
  unsigned int mutex_val = 0;                                                \
  unsigned int inc_wait = 1;                                                 \
  do                                                                         \
  {                                                                          \
    if (atomic_compare_exchange_strong(&(mutex)->val, &mutex_val,            \
                          (mutex_val + inc_wait) | BSDLIC_MUTEX_OWNED_FLAG)) \
    {                                                                        \
      if (0 == (mutex_val & BSDLIC_MUTEX_OWNED_FLAG))                        \
      {                                                                      \
        break;                                                               \
      }                                                                      \
      (void) bsdlic_futex (&(mutex)->val,                                    \
            (mutex)->shared?FUTEX_WAIT:FUTEX_WAIT_PRIVATE,                   \
            mutex_val + inc_wait,                                            \
            0, 0, 0);                                                        \
       mutex_val = atomic_load(&(mutex)->val);                               \
       inc_wait = 0;                                                         \
    }                                                                        \
  } while (1);                                                               \
})
 
#endif /* MUTEX_LIB_H_INCLUDED */
