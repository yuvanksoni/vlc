/*****************************************************************************
 * thread.c : Win32 back-end for LibVLC
 *****************************************************************************
 * Copyright (C) 1999-2016 VLC authors and VideoLAN
 *
 * Authors: Jean-Marc Dressler <polux@via.ecp.fr>
 *          Samuel Hocevar <sam@zoy.org>
 *          Gildas Bazin <gbazin@netcourrier.com>
 *          Clément Sténac
 *          Rémi Denis-Courmont
 *          Pierre Ynard
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_atomic.h>

#include "libvlc.h"
#include <stdarg.h>
#include <assert.h>
#include <limits.h>
#include <errno.h>
#include <time.h>

/*** Static mutex and condition variable ***/
static vlc_mutex_t super_mutex;
static vlc_cond_t  super_variable;

#define IS_INTERRUPTIBLE (!VLC_WINSTORE_APP || _WIN32_WINNT >= 0x0A00)

/*** Threads ***/
static DWORD thread_key;

struct vlc_thread
{
    HANDLE         id;

    bool           killable;
    atomic_bool    killed;
    vlc_cleanup_t *cleaners;

    void        *(*entry) (void *);
    void          *data;

    struct
    {
        atomic_int      *addr;
        CRITICAL_SECTION lock;
    } wait;
};

/*** Common helpers ***/
#if !IS_INTERRUPTIBLE
static bool isCancelled(void);
#endif

static DWORD vlc_WaitForMultipleObjects (DWORD count, const HANDLE *handles,
                                         DWORD delay)
{
    DWORD ret;
    if (count == 0)
    {
#if !IS_INTERRUPTIBLE
        do {
            DWORD new_delay = 50;
            if (new_delay > delay)
                new_delay = delay;
            ret = SleepEx (new_delay, TRUE);
            if (delay != INFINITE)
                delay -= new_delay;
            if (isCancelled())
                ret = WAIT_IO_COMPLETION;
        } while (delay && ret == 0);
#else
        ret = SleepEx (delay, TRUE);
#endif

        if (ret == 0)
            ret = WAIT_TIMEOUT;
    }
    else {
#if !IS_INTERRUPTIBLE
        do {
            DWORD new_delay = 50;
            if (new_delay > delay)
                new_delay = delay;
            ret = WaitForMultipleObjectsEx (count, handles, FALSE, new_delay, TRUE);
            if (delay != INFINITE)
                delay -= new_delay;
            if (isCancelled())
                ret = WAIT_IO_COMPLETION;
        } while (delay && ret == WAIT_TIMEOUT);
#else
        ret = WaitForMultipleObjectsEx (count, handles, FALSE, delay, TRUE);
#endif
    }

    /* We do not abandon objects... this would be a bug */
    assert (ret < WAIT_ABANDONED_0 || WAIT_ABANDONED_0 + count - 1 < ret);

    if (unlikely(ret == WAIT_FAILED))
        abort (); /* We are screwed! */
    return ret;
}

static DWORD vlc_WaitForSingleObject (HANDLE handle, DWORD delay)
{
    return vlc_WaitForMultipleObjects (1, &handle, delay);
}

static DWORD vlc_Sleep (DWORD delay)
{
    DWORD ret = vlc_WaitForMultipleObjects (0, NULL, delay);
    return (ret != WAIT_TIMEOUT) ? ret : 0;
}


/*** Mutexes ***/
void vlc_mutex_init( vlc_mutex_t *p_mutex )
{
    /* This creates a recursive mutex. This is OK as fast mutexes have
     * no defined behavior in case of recursive locking. */
    InitializeCriticalSection (&p_mutex->mutex);
    p_mutex->dynamic = true;
}

void vlc_mutex_init_recursive( vlc_mutex_t *p_mutex )
{
    InitializeCriticalSection( &p_mutex->mutex );
    p_mutex->dynamic = true;
}


void vlc_mutex_destroy (vlc_mutex_t *p_mutex)
{
    assert (p_mutex->dynamic);
    DeleteCriticalSection (&p_mutex->mutex);
}

void vlc_mutex_lock (vlc_mutex_t *p_mutex)
{
    if (!p_mutex->dynamic)
    {   /* static mutexes */
        int canc = vlc_savecancel ();
        assert (p_mutex != &super_mutex); /* this one cannot be static */

        vlc_mutex_lock (&super_mutex);
        while (p_mutex->locked)
        {
            p_mutex->contention++;
            vlc_cond_wait (&super_variable, &super_mutex);
            p_mutex->contention--;
        }
        p_mutex->locked = true;
        vlc_mutex_unlock (&super_mutex);
        vlc_restorecancel (canc);
        return;
    }

    EnterCriticalSection (&p_mutex->mutex);
}

int vlc_mutex_trylock (vlc_mutex_t *p_mutex)
{
    if (!p_mutex->dynamic)
    {   /* static mutexes */
        int ret = EBUSY;

        assert (p_mutex != &super_mutex); /* this one cannot be static */
        vlc_mutex_lock (&super_mutex);
        if (!p_mutex->locked)
        {
            p_mutex->locked = true;
            ret = 0;
        }
        vlc_mutex_unlock (&super_mutex);
        return ret;
    }

    return TryEnterCriticalSection (&p_mutex->mutex) ? 0 : EBUSY;
}

void vlc_mutex_unlock (vlc_mutex_t *p_mutex)
{
    if (!p_mutex->dynamic)
    {   /* static mutexes */
        assert (p_mutex != &super_mutex); /* this one cannot be static */

        vlc_mutex_lock (&super_mutex);
        assert (p_mutex->locked);
        p_mutex->locked = false;
        if (p_mutex->contention)
            vlc_cond_broadcast (&super_variable);
        vlc_mutex_unlock (&super_mutex);
        return;
    }

    LeaveCriticalSection (&p_mutex->mutex);
}

/*** Semaphore ***/
void vlc_sem_init (vlc_sem_t *sem, unsigned value)
{
    *sem = CreateSemaphore (NULL, value, 0x7fffffff, NULL);
    if (*sem == NULL)
        abort ();
}

void vlc_sem_destroy (vlc_sem_t *sem)
{
    CloseHandle (*sem);
}

int vlc_sem_post (vlc_sem_t *sem)
{
    ReleaseSemaphore (*sem, 1, NULL);
    return 0; /* FIXME */
}

void vlc_sem_wait (vlc_sem_t *sem)
{
    DWORD result;

    do
    {
        vlc_testcancel ();
        result = vlc_WaitForSingleObject (*sem, INFINITE);
    }
    while (result == WAIT_IO_COMPLETION);
}

/*** Thread-specific variables (TLS) ***/
struct vlc_threadvar
{
    DWORD                 id;
    void                (*destroy) (void *);
    struct vlc_threadvar *prev;
    struct vlc_threadvar *next;
} *vlc_threadvar_last = NULL;

int vlc_threadvar_create (vlc_threadvar_t *p_tls, void (*destr) (void *))
{
    struct vlc_threadvar *var = malloc (sizeof (*var));
    if (unlikely(var == NULL))
        return errno;

    var->id = TlsAlloc();
    if (var->id == TLS_OUT_OF_INDEXES)
    {
        free (var);
        return EAGAIN;
    }
    var->destroy = destr;
    var->next = NULL;
    *p_tls = var;

    vlc_mutex_lock (&super_mutex);
    var->prev = vlc_threadvar_last;
    if (var->prev)
        var->prev->next = var;

    vlc_threadvar_last = var;
    vlc_mutex_unlock (&super_mutex);
    return 0;
}

void vlc_threadvar_delete (vlc_threadvar_t *p_tls)
{
    struct vlc_threadvar *var = *p_tls;

    vlc_mutex_lock (&super_mutex);
    if (var->prev != NULL)
        var->prev->next = var->next;

    if (var->next != NULL)
        var->next->prev = var->prev;
    else
        vlc_threadvar_last = var->prev;

    vlc_mutex_unlock (&super_mutex);

    TlsFree (var->id);
    free (var);
}

int vlc_threadvar_set (vlc_threadvar_t key, void *value)
{
    int saved = GetLastError ();
    int val = TlsSetValue (key->id, value) ? ENOMEM : 0;

    if (val == 0)
        SetLastError(saved);
    return val;
}

void *vlc_threadvar_get (vlc_threadvar_t key)
{
    int saved = GetLastError ();
    void *value = TlsGetValue (key->id);

    SetLastError(saved);
    return value;
}

static void vlc_threadvars_cleanup(void)
{
    vlc_threadvar_t key;
retry:
    /* TODO: use RW lock or something similar */
    vlc_mutex_lock(&super_mutex);
    for (key = vlc_threadvar_last; key != NULL; key = key->prev)
    {
        void *value = vlc_threadvar_get(key);
        if (value != NULL && key->destroy != NULL)
        {
            vlc_mutex_unlock(&super_mutex);
            vlc_threadvar_set(key, NULL);
            key->destroy(value);
            goto retry;
        }
    }
    vlc_mutex_unlock(&super_mutex);
}

/*** Condition variables (low-level) ***/
#if (_WIN32_WINNT < _WIN32_WINNT_VISTA)
static VOID (WINAPI *InitializeConditionVariable_)(PCONDITION_VARIABLE);
#define InitializeConditionVariable InitializeConditionVariable_
static BOOL (WINAPI *SleepConditionVariableCS_)(PCONDITION_VARIABLE,
                                                PCRITICAL_SECTION, DWORD);
#define SleepConditionVariableCS SleepConditionVariableCS_
static VOID (WINAPI *WakeAllConditionVariable_)(PCONDITION_VARIABLE);
#define WakeAllConditionVariable WakeAllConditionVariable_

static void WINAPI DummyConditionVariable(CONDITION_VARIABLE *cv)
{
    (void) cv;
}

static BOOL WINAPI SleepConditionVariableFallback(CONDITION_VARIABLE *cv,
                                                  CRITICAL_SECTION *cs,
                                                  DWORD ms)
{
    (void) cv;
    LeaveCriticalSection(cs);
    SleepEx(0, TRUE);
    EnterCriticalSection(cs);
    return ms != 0;
}
#endif

/*** Futeces^WAddress waits ***/
#if (_WIN32_WINNT < _WIN32_WINNT_WIN8)
static BOOL (WINAPI *WaitOnAddress_)(VOID volatile *, PVOID, SIZE_T, DWORD);
#define WaitOnAddress (*WaitOnAddress_)
static VOID (WINAPI *WakeByAddressAll_)(PVOID);
#define WakeByAddressAll (*WakeByAddressAll_)
static VOID (WINAPI *WakeByAddressSingle_)(PVOID);
#define WakeByAddressSingle (*WakeByAddressSingle_)

static struct wait_addr_bucket
{
    CRITICAL_SECTION lock;
    CONDITION_VARIABLE wait;
} wait_addr_buckets[32];

static struct wait_addr_bucket *wait_addr_get_bucket(void volatile *addr)
{
    uintptr_t u = (uintptr_t)addr;

    return wait_addr_buckets + ((u >> 3) % ARRAY_SIZE(wait_addr_buckets));
}

static void vlc_wait_addr_init(void)
{
    for (size_t i = 0; i < ARRAY_SIZE(wait_addr_buckets); i++)
    {
        struct wait_addr_bucket *bucket = wait_addr_buckets + i;

        InitializeCriticalSection(&bucket->lock);
        InitializeConditionVariable(&bucket->wait);
    }
}

static void vlc_wait_addr_deinit(void)
{
    for (size_t i = 0; i < ARRAY_SIZE(wait_addr_buckets); i++)
    {
        struct wait_addr_bucket *bucket = wait_addr_buckets + i;

        DeleteCriticalSection(&bucket->lock);
    }
}

static BOOL WINAPI WaitOnAddressFallback(void volatile *addr, void *value,
                                         SIZE_T size, DWORD ms)
{
    struct wait_addr_bucket *bucket = wait_addr_get_bucket(addr);
    uint64_t futex, val = 0;
    BOOL ret = 0;

    EnterCriticalSection(&bucket->lock);

    switch (size)
    {
        case 1:
            futex = atomic_load_explicit((atomic_char *)addr,
                                         memory_order_relaxed);
            val = *(const char *)value;
            break;
        case 2:
            futex = atomic_load_explicit((atomic_short *)addr,
                                         memory_order_relaxed);
            val = *(const short *)value;
            break;
        case 4:
            futex = atomic_load_explicit((atomic_int *)addr,
                                         memory_order_relaxed);
            val = *(const int *)value;
            break;
        case 8:
            futex = atomic_load_explicit((atomic_llong *)addr,
                                         memory_order_relaxed);
            val = *(const long long *)value;
            break;
        default:
            vlc_assert_unreachable();
    }

    if (futex == val)
        ret = SleepConditionVariableCS(&bucket->wait, &bucket->lock, ms);

    LeaveCriticalSection(&bucket->lock);
    return ret;
}

static void WINAPI WakeByAddressFallback(void *addr)
{
    struct wait_addr_bucket *bucket = wait_addr_get_bucket(addr);

    /* Acquire the bucket critical section (only) to enforce proper sequencing.
     * The critical section does not protect any actual memory object. */
    EnterCriticalSection(&bucket->lock);
    /* No other threads can hold the lock for this bucket while it is held
     * here. Thus any other thread either:
     * - is already sleeping in SleepConditionVariableCS(), and to be woken up
     *   by the following WakeAllConditionVariable(), or
     * - has yet to retrieve the value at the wait address (with the
     *   'switch (size)' block). */
    LeaveCriticalSection(&bucket->lock);
    /* At this point, other threads can retrieve the value at the wait address.
     * But the value will have already been changed by our call site, thus
     * (futex == val) will be false, and the threads will not go to sleep. */

    /* Wake up any thread that was already sleeping. Since there are more than
     * one wait address per bucket, all threads must be woken up :-/ */
    WakeAllConditionVariable(&bucket->wait);
}
#endif

void vlc_addr_wait(void *addr, unsigned val)
{
    WaitOnAddress(addr, &val, sizeof (val), -1);
}

bool vlc_addr_timedwait(void *addr, unsigned val, mtime_t delay)
{
    delay = (delay + 999) / 1000;

    if (delay > 0x7fffffff)
    {
        WaitOnAddress(addr, &val, sizeof (val), 0x7fffffff);
        return true; /* woke up early, claim spurious wake-up */
    }

    return WaitOnAddress(addr, &val, sizeof (val), delay);
}

void vlc_addr_signal(void *addr)
{
    WakeByAddressSingle(addr);
}

void vlc_addr_broadcast(void *addr)
{
    WakeByAddressAll(addr);
}

/*** Threads ***/
#if !IS_INTERRUPTIBLE
static bool isCancelled(void)
{
    struct vlc_thread *th = vlc_thread_self();
    if (th == NULL)
        return false; /* Main thread - cannot be cancelled anyway */

    return atomic_load(&th->killed);
}
#endif

static void vlc_thread_destroy(vlc_thread_t th)
{
    DeleteCriticalSection(&th->wait.lock);
    free(th);
}

static unsigned __stdcall vlc_entry (void *p)
{
    struct vlc_thread *th = p;

    TlsSetValue(thread_key, th);
    th->killable = true;
    th->data = th->entry (th->data);
    TlsSetValue(thread_key, NULL);

    if (th->id == NULL) /* Detached thread */
        vlc_thread_destroy(th);
    return 0;
}

static int vlc_clone_attr (vlc_thread_t *p_handle, bool detached,
                           void *(*entry) (void *), void *data, int priority)
{
    struct vlc_thread *th = malloc (sizeof (*th));
    if (unlikely(th == NULL))
        return ENOMEM;
    th->entry = entry;
    th->data = data;
    th->killable = false; /* not until vlc_entry() ! */
    atomic_init(&th->killed, false);
    th->cleaners = NULL;
    th->wait.addr = NULL;
    InitializeCriticalSection(&th->wait.lock);

    /* When using the MSVCRT C library you have to use the _beginthreadex
     * function instead of CreateThread, otherwise you'll end up with
     * memory leaks and the signal functions not working (see Microsoft
     * Knowledge Base, article 104641) */
    uintptr_t h = _beginthreadex (NULL, 0, vlc_entry, th, 0, NULL);
    if (h == 0)
    {
        int err = errno;
        free (th);
        return err;
    }

    if (detached)
    {
        CloseHandle((HANDLE)h);
        th->id = NULL;
    }
    else
        th->id = (HANDLE)h;

    if (p_handle != NULL)
        *p_handle = th;

    if (priority)
        SetThreadPriority (th->id, priority);

    return 0;
}

int vlc_clone (vlc_thread_t *p_handle, void *(*entry) (void *),
                void *data, int priority)
{
    return vlc_clone_attr (p_handle, false, entry, data, priority);
}

void vlc_join (vlc_thread_t th, void **result)
{
    do
        vlc_testcancel ();
    while (vlc_WaitForSingleObject (th->id, INFINITE) == WAIT_IO_COMPLETION);

    if (result != NULL)
        *result = th->data;
    CloseHandle (th->id);
    vlc_thread_destroy(th);
}

int vlc_clone_detach (vlc_thread_t *p_handle, void *(*entry) (void *),
                      void *data, int priority)
{
    vlc_thread_t th;
    if (p_handle == NULL)
        p_handle = &th;

    return vlc_clone_attr (p_handle, true, entry, data, priority);
}

vlc_thread_t vlc_thread_self (void)
{
    return TlsGetValue(thread_key);
}

unsigned long vlc_thread_id (void)
{
    return GetCurrentThreadId ();
}

int vlc_set_priority (vlc_thread_t th, int priority)
{
    if (!SetThreadPriority (th->id, priority))
        return VLC_EGENERIC;
    return VLC_SUCCESS;
}

/*** Thread cancellation ***/

#if IS_INTERRUPTIBLE
/* APC procedure for thread cancellation */
static void CALLBACK vlc_cancel_self (ULONG_PTR self)
{
    (void) self;
}
#endif

void vlc_cancel (vlc_thread_t th)
{
    atomic_store_explicit(&th->killed, true, memory_order_relaxed);

    EnterCriticalSection(&th->wait.lock);
    if (th->wait.addr != NULL)
    {
        atomic_fetch_and_explicit(th->wait.addr, -2, memory_order_relaxed);
        vlc_addr_broadcast(th->wait.addr);
    }
    LeaveCriticalSection(&th->wait.lock);

#if IS_INTERRUPTIBLE
    QueueUserAPC (vlc_cancel_self, th->id, (uintptr_t)th);
#endif
}

int vlc_savecancel (void)
{
    struct vlc_thread *th = vlc_thread_self();
    if (th == NULL)
        return false; /* Main thread - cannot be cancelled anyway */

    int state = th->killable;
    th->killable = false;
    return state;
}

void vlc_restorecancel (int state)
{
    struct vlc_thread *th = vlc_thread_self();
    assert (state == false || state == true);

    if (th == NULL)
        return; /* Main thread - cannot be cancelled anyway */

    assert (!th->killable);
    th->killable = state != 0;
}

void vlc_testcancel (void)
{
    struct vlc_thread *th = vlc_thread_self();
    if (th == NULL)
        return; /* Main thread - cannot be cancelled anyway */
    if (!th->killable)
        return;
    if (!atomic_load_explicit(&th->killed, memory_order_relaxed))
        return;

    th->killable = true; /* Do not re-enter cancellation cleanup */

    for (vlc_cleanup_t *p = th->cleaners; p != NULL; p = p->next)
        p->proc (p->data);

    th->data = NULL; /* TODO: special value? */
    if (th->id == NULL) /* Detached thread */
        vlc_thread_destroy(th);
    _endthreadex(0);
}

void vlc_control_cancel (int cmd, ...)
{
    /* NOTE: This function only modifies thread-specific data, so there is no
     * need to lock anything. */
    va_list ap;

    struct vlc_thread *th = vlc_thread_self();
    if (th == NULL)
        return; /* Main thread - cannot be cancelled anyway */

    va_start (ap, cmd);
    switch (cmd)
    {
        case VLC_CLEANUP_PUSH:
        {
            /* cleaner is a pointer to the caller stack, no need to allocate
             * and copy anything. As a nice side effect, this cannot fail. */
            vlc_cleanup_t *cleaner = va_arg (ap, vlc_cleanup_t *);
            cleaner->next = th->cleaners;
            th->cleaners = cleaner;
            break;
        }

        case VLC_CLEANUP_POP:
        {
            th->cleaners = th->cleaners->next;
            break;
        }

        case VLC_CANCEL_ADDR_SET:
        {
            void *addr = va_arg(ap, void *);

            EnterCriticalSection(&th->wait.lock);
            th->wait.addr = addr;
            LeaveCriticalSection(&th->wait.lock);
            break;
        }

        case VLC_CANCEL_ADDR_CLEAR:
        {
            void *addr = va_arg(ap, void *);

            EnterCriticalSection(&th->wait.lock);
            assert(th->wait.addr == addr);
            th->wait.addr = NULL;
            LeaveCriticalSection(&th->wait.lock);
            break;
        }
    }
    va_end (ap);
}

/*** Clock ***/
static union
{
#if (_WIN32_WINNT < 0x0601)
    struct
    {
        BOOL (*query) (PULONGLONG);
    } interrupt;
#endif
#if (_WIN32_WINNT < 0x0600)
    struct
    {
        ULONGLONG (*get) (void);
    } tick;
#endif
    struct
    {
        LARGE_INTEGER freq;
    } perf;
} clk;

static mtime_t mdate_interrupt (void)
{
    ULONGLONG ts;
    BOOL ret;

#if (_WIN32_WINNT >= 0x0601)
    ret = QueryUnbiasedInterruptTime (&ts);
#else
    ret = clk.interrupt.query (&ts);
#endif
    if (unlikely(!ret))
        abort ();

    /* hundreds of nanoseconds */
    static_assert ((10000000 % CLOCK_FREQ) == 0, "Broken frequencies ratio");
    return ts / (10000000 / CLOCK_FREQ);
}

static mtime_t mdate_tick (void)
{
#if (_WIN32_WINNT >= 0x0600)
    ULONGLONG ts = GetTickCount64 ();
#else
    ULONGLONG ts = clk.tick.get ();
#endif

    /* milliseconds */
    static_assert ((CLOCK_FREQ % 1000) == 0, "Broken frequencies ratio");
    return ts * (CLOCK_FREQ / 1000);
}
#if !VLC_WINSTORE_APP
#include <mmsystem.h>
static mtime_t mdate_multimedia (void)
{
     DWORD ts = timeGetTime ();

    /* milliseconds */
    static_assert ((CLOCK_FREQ % 1000) == 0, "Broken frequencies ratio");
    return ts * (CLOCK_FREQ / 1000);
}
#endif

static mtime_t mdate_perf (void)
{
    /* We don't need the real date, just the value of a high precision timer */
    LARGE_INTEGER counter;
    if (!QueryPerformanceCounter (&counter))
        abort ();

    /* Convert to from (1/freq) to microsecond resolution */
    /* We need to split the division to avoid 63-bits overflow */
    lldiv_t d = lldiv (counter.QuadPart, clk.perf.freq.QuadPart);

    return (d.quot * 1000000) + ((d.rem * 1000000) / clk.perf.freq.QuadPart);
}

static mtime_t mdate_wall (void)
{
    FILETIME ts;
    ULARGE_INTEGER s;

#if (_WIN32_WINNT >= 0x0602) && !VLC_WINSTORE_APP
    GetSystemTimePreciseAsFileTime (&ts);
#else
    GetSystemTimeAsFileTime (&ts);
#endif
    s.LowPart = ts.dwLowDateTime;
    s.HighPart = ts.dwHighDateTime;
    /* hundreds of nanoseconds */
    static_assert ((10000000 % CLOCK_FREQ) == 0, "Broken frequencies ratio");
    return s.QuadPart / (10000000 / CLOCK_FREQ);
}

static CRITICAL_SECTION clock_lock;
static bool clock_used_early = false;

static mtime_t mdate_default(void)
{
    EnterCriticalSection(&clock_lock);
    if (!clock_used_early)
    {
        if (!QueryPerformanceFrequency(&clk.perf.freq))
            abort();
        clock_used_early = true;
    }
    LeaveCriticalSection(&clock_lock);

    return mdate_perf();
}

static mtime_t (*mdate_selected) (void) = mdate_default;

mtime_t mdate (void)
{
    return mdate_selected ();
}

#undef mwait
void mwait (mtime_t deadline)
{
    mtime_t delay;

    vlc_testcancel();
    while ((delay = (deadline - mdate())) > 0)
    {
        delay = (delay + 999) / 1000;
        if (unlikely(delay > 0x7fffffff))
            delay = 0x7fffffff;
        vlc_Sleep (delay);
        vlc_testcancel();
    }
}

#undef msleep
void msleep (mtime_t delay)
{
    mwait (mdate () + delay);
}

static void SelectClockSource (vlc_object_t *obj)
{
    EnterCriticalSection (&clock_lock);
    if (mdate_selected != mdate_default)
    {
        LeaveCriticalSection (&clock_lock);
        return;
    }

    assert(!clock_used_early);

#if VLC_WINSTORE_APP
    const char *name = "perf";
#else
    const char *name = "multimedia";
#endif
    char *str = var_InheritString (obj, "clock-source");
    if (str != NULL)
        name = str;
    if (!strcmp (name, "interrupt"))
    {
        msg_Dbg (obj, "using interrupt time as clock source");
#if (_WIN32_WINNT < 0x0601)
        HANDLE h = GetModuleHandle (_T("kernel32.dll"));
        if (unlikely(h == NULL))
            abort ();
        clk.interrupt.query = (void *)GetProcAddress (h,
                                                      "QueryUnbiasedInterruptTime");
        if (unlikely(clk.interrupt.query == NULL))
            abort ();
#endif
        mdate_selected = mdate_interrupt;
    }
    else
    if (!strcmp (name, "tick"))
    {
        msg_Dbg (obj, "using Windows time as clock source");
#if (_WIN32_WINNT < 0x0600)
        HANDLE h = GetModuleHandle (_T("kernel32.dll"));
        if (unlikely(h == NULL))
            abort ();
        clk.tick.get = (void *)GetProcAddress (h, "GetTickCount64");
        if (unlikely(clk.tick.get == NULL))
            abort ();
#endif
        mdate_selected = mdate_tick;
    }
#if !VLC_WINSTORE_APP
    else
    if (!strcmp (name, "multimedia"))
    {
        TIMECAPS caps;

        msg_Dbg (obj, "using multimedia timers as clock source");
        if (timeGetDevCaps (&caps, sizeof (caps)) != MMSYSERR_NOERROR)
            abort ();
        msg_Dbg (obj, " min period: %u ms, max period: %u ms",
                 caps.wPeriodMin, caps.wPeriodMax);
        mdate_selected = mdate_multimedia;
    }
#endif
    else
    if (!strcmp (name, "perf"))
    {
        msg_Dbg (obj, "using performance counters as clock source");
        if (!QueryPerformanceFrequency (&clk.perf.freq))
            abort ();
        msg_Dbg (obj, " frequency: %llu Hz", clk.perf.freq.QuadPart);
        mdate_selected = mdate_perf;
    }
    else
    if (!strcmp (name, "wall"))
    {
        msg_Dbg (obj, "using system time as clock source");
        mdate_selected = mdate_wall;
    }
    else
    {
        msg_Err (obj, "invalid clock source \"%s\"", name);
        abort ();
    }
    LeaveCriticalSection (&clock_lock);
    free (str);
}

size_t EnumClockSource (vlc_object_t *obj, const char *var,
                        char ***vp, char ***np)
{
    const size_t max = 6;
    char **values = xmalloc (sizeof (*values) * max);
    char **names = xmalloc (sizeof (*names) * max);
    size_t n = 0;

#if (_WIN32_WINNT < 0x0601)
    DWORD version = LOWORD(GetVersion());
    version = (LOBYTE(version) << 8) | (HIBYTE(version) << 0);
#endif

    values[n] = xstrdup ("");
    names[n] = xstrdup (_("Auto"));
    n++;
#if (_WIN32_WINNT < 0x0601)
    if (version >= 0x0601)
#endif
    {
        values[n] = xstrdup ("interrupt");
        names[n] = xstrdup ("Interrupt time");
        n++;
    }
#if (_WIN32_WINNT < 0x0600)
    if (version >= 0x0600)
#endif
    {
        values[n] = xstrdup ("tick");
        names[n] = xstrdup ("Windows time");
        n++;
    }
#if !VLC_WINSTORE_APP
    values[n] = xstrdup ("multimedia");
    names[n] = xstrdup ("Multimedia timers");
    n++;
#endif
    values[n] = xstrdup ("perf");
    names[n] = xstrdup ("Performance counters");
    n++;
    values[n] = xstrdup ("wall");
    names[n] = xstrdup ("System time (DANGEROUS!)");
    n++;

    *vp = values;
    *np = names;
    (void) obj; (void) var;
    return n;
}


/*** Timers ***/
struct vlc_timer
{
    HANDLE handle;
    void (*func) (void *);
    void *data;
};

static void CALLBACK vlc_timer_do (void *val, BOOLEAN timeout)
{
    struct vlc_timer *timer = val;

    assert (timeout);
    timer->func (timer->data);
}

int vlc_timer_create (vlc_timer_t *id, void (*func) (void *), void *data)
{
    struct vlc_timer *timer = malloc (sizeof (*timer));

    if (timer == NULL)
        return ENOMEM;
    timer->func = func;
    timer->data = data;
    timer->handle = INVALID_HANDLE_VALUE;
    *id = timer;
    return 0;
}

void vlc_timer_destroy (vlc_timer_t timer)
{
#if !VLC_WINSTORE_APP
    if (timer->handle != INVALID_HANDLE_VALUE)
        DeleteTimerQueueTimer (NULL, timer->handle, INVALID_HANDLE_VALUE);
#endif
    free (timer);
}

void vlc_timer_schedule (vlc_timer_t timer, bool absolute,
                         mtime_t value, mtime_t interval)
{
    if (timer->handle != INVALID_HANDLE_VALUE)
    {
#if !VLC_WINSTORE_APP
        DeleteTimerQueueTimer (NULL, timer->handle, INVALID_HANDLE_VALUE);
#endif
        timer->handle = INVALID_HANDLE_VALUE;
    }
    if (value == 0)
        return; /* Disarm */

    if (absolute)
    {
        value -= mdate ();
        if (value < 0)
            value = 0;
    }
    value = (value + 999) / 1000;
    interval = (interval + 999) / 1000;

#if !VLC_WINSTORE_APP
    if (!CreateTimerQueueTimer (&timer->handle, NULL, vlc_timer_do, timer,
                                value, interval, WT_EXECUTEDEFAULT))
#endif
        abort ();
}

unsigned vlc_timer_getoverrun (vlc_timer_t timer)
{
    (void)timer;
    return 0;
}


/*** CPU ***/
unsigned vlc_GetCPUCount (void)
{
    SYSTEM_INFO systemInfo;

    GetNativeSystemInfo(&systemInfo);

    return systemInfo.dwNumberOfProcessors;
}


/*** Initialization ***/
void vlc_threads_setup (libvlc_int_t *p_libvlc)
{
    SelectClockSource (VLC_OBJECT(p_libvlc));
}

#define LOOKUP(s) (((s##_) = (void *)GetProcAddress(h, #s)) != NULL)

extern vlc_rwlock_t config_lock;
BOOL WINAPI DllMain (HINSTANCE, DWORD, LPVOID);

BOOL WINAPI DllMain (HINSTANCE hinstDll, DWORD fdwReason, LPVOID lpvReserved)
{
    (void) hinstDll;
    (void) lpvReserved;

    switch (fdwReason)
    {
        case DLL_PROCESS_ATTACH:
        {
#if (_WIN32_WINNT < _WIN32_WINNT_WIN8)
            HANDLE h = GetModuleHandle(TEXT("kernel32.dll"));
            if (unlikely(h == NULL))
                return FALSE;

            if (!LOOKUP(WaitOnAddress)
             || !LOOKUP(WakeByAddressAll) || !LOOKUP(WakeByAddressSingle))
            {
# if (_WIN32_WINNT < _WIN32_WINNT_VISTA)
                if (!LOOKUP(InitializeConditionVariable)
                 || !LOOKUP(SleepConditionVariableCS)
                 || !LOOKUP(WakeAllConditionVariable))
                {
                    InitializeConditionVariable_ = DummyConditionVariable;
                    SleepConditionVariableCS_ = SleepConditionVariableFallback;
                    WakeAllConditionVariable_ = DummyConditionVariable;
                }
# endif
                vlc_wait_addr_init();
                WaitOnAddress_ = WaitOnAddressFallback;
                WakeByAddressAll_ = WakeByAddressFallback;
                WakeByAddressSingle_ = WakeByAddressFallback;
            }
#endif
            thread_key = TlsAlloc();
            if (unlikely(thread_key == TLS_OUT_OF_INDEXES))
                return FALSE;
            InitializeCriticalSection (&clock_lock);
            vlc_mutex_init (&super_mutex);
            vlc_cond_init (&super_variable);
            vlc_rwlock_init (&config_lock);
            vlc_CPU_init ();
            break;
        }

        case DLL_PROCESS_DETACH:
            vlc_rwlock_destroy (&config_lock);
            vlc_cond_destroy (&super_variable);
            vlc_mutex_destroy (&super_mutex);
            DeleteCriticalSection (&clock_lock);
            TlsFree(thread_key);
#if (_WIN32_WINNT < _WIN32_WINNT_WIN8)
            if (WaitOnAddress_ == WaitOnAddressFallback)
                vlc_wait_addr_deinit();
#endif
            break;

        case DLL_THREAD_DETACH:
            vlc_threadvars_cleanup();
            break;
    }
    return TRUE;
}
