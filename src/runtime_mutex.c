/* Opaque PS4 mutex and condition variable handles over the host's own primitives: pthread
 * mutexes/condition variables on Linux; on Windows an SRW lock with owner and depth (the PS4
 * error-checking, recursive and normal types) and a condition variable. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
enum { TYPE_ERRORCHECK = 1, TYPE_RECURSIVE = 2, TYPE_NORMAL = 3 }; /* 4: adaptive, error-checking here */
#ifdef _WIN32
typedef struct { SRWLOCK lock; DWORD owner; unsigned depth; int type; } NativeMutex;
typedef CONDITION_VARIABLE NativeCond;
static int native_init(NativeMutex *m, int type) {
    InitializeSRWLock(&m->lock); m->owner = 0; m->depth = 0; m->type = type; return 0;
}
static int native_destroy(NativeMutex *m) { return __atomic_load_n(&m->owner, __ATOMIC_RELAXED) ? EBUSY : 0; }
static DWORD owner(const NativeMutex *m) { return __atomic_load_n(&m->owner, __ATOMIC_RELAXED); }
static void take(NativeMutex *m, DWORD self) { __atomic_store_n(&m->owner, self, __ATOMIC_RELAXED); m->depth = 1; }
/* Relocking by the owner: recursion, an error, or (normal type) a deadlock as on POSIX. */
static int relock(NativeMutex *m) {
    if (m->type == TYPE_RECURSIVE) return m->depth == ~0u ? EAGAIN : (++m->depth, 0);
    return m->type == TYPE_NORMAL ? -1 : EDEADLK;
}
static int native_lock(NativeMutex *m) {
    DWORD self = GetCurrentThreadId();
    if (owner(m) == self) { int e = relock(m); if (e >= 0) return e; }
    AcquireSRWLockExclusive(&m->lock);
    take(m, self);
    return 0;
}
static int native_trylock(NativeMutex *m) {
    DWORD self = GetCurrentThreadId();
    if (owner(m) == self) return m->type == TYPE_RECURSIVE ? relock(m) : EBUSY;
    if (!TryAcquireSRWLockExclusive(&m->lock)) return EBUSY;
    take(m, self);
    return 0;
}
/* SRW locks have no timed acquire: retry until the realtime deadline (uncontended in practice). */
static int native_timedlock(NativeMutex *m, uint64_t deadline) {
    DWORD self = GetCurrentThreadId();
    if (owner(m) == self) { int e = relock(m); if (e >= 0) return e; }
    for (unsigned attempt = 0;; ++attempt) {
        if (TryAcquireSRWLockExclusive(&m->lock)) { take(m, self); return 0; }
        if (host_realtime_ns() >= deadline) return ETIMEDOUT;
        if (attempt < 64) YieldProcessor(); else if (attempt < 128) SwitchToThread(); else Sleep(1);
    }
}
static int native_unlock(NativeMutex *m) {
    DWORD self = GetCurrentThreadId(), held = owner(m);
    /* A normal mutex may be unlocked by another thread (glibc does not check its owner). */
    if (held != self && (m->type != TYPE_NORMAL || !held)) return EPERM;
    if (held == self && --m->depth) return 0;
    __atomic_store_n(&m->owner, 0, __ATOMIC_RELAXED); m->depth = 0;
    ReleaseSRWLockExclusive(&m->lock);
    return 0;
}
static int native_cond_init(NativeCond *c) { InitializeConditionVariable(c); return 0; }
static int native_cond_destroy(NativeCond *c) { (void)c; return 0; }
/* deadline: realtime nanoseconds, 0 for none. The wait gives up the whole recursion. */
static int native_cond_wait(NativeCond *c, NativeMutex *m, uint64_t deadline) {
    DWORD self = GetCurrentThreadId(), ms = INFINITE;
    if (owner(m) != self) return EPERM;
    if (deadline) {
        uint64_t now = host_realtime_ns();
        if (now >= deadline) return ETIMEDOUT;
        uint64_t left = (deadline - now + 999999) / 1000000;
        ms = left >= INFINITE ? INFINITE - 1 : (DWORD)left;
    }
    unsigned depth = m->depth;
    __atomic_store_n(&m->owner, 0, __ATOMIC_RELAXED); m->depth = 0;
    BOOL woken = SleepConditionVariableSRW(c, &m->lock, ms, 0);
    DWORD error = woken ? 0 : GetLastError();
    __atomic_store_n(&m->owner, self, __ATOMIC_RELAXED); m->depth = depth;
    return woken ? 0 : error == ERROR_TIMEOUT ? ETIMEDOUT : EINVAL;
}
static int native_cond_signal(NativeCond *c) { WakeConditionVariable(c); return 0; }
static int native_cond_broadcast(NativeCond *c) { WakeAllConditionVariable(c); return 0; }
#else
#include <pthread.h>
typedef pthread_mutex_t NativeMutex;
typedef pthread_cond_t NativeCond;
static int native_init(NativeMutex *m, int type) {
    pthread_mutexattr_t attr;
    int e = pthread_mutexattr_init(&attr);
    if (e) return e;
    int native_type = type == TYPE_RECURSIVE ? PTHREAD_MUTEX_RECURSIVE : type == TYPE_NORMAL ? PTHREAD_MUTEX_NORMAL : PTHREAD_MUTEX_ERRORCHECK;
    e = pthread_mutexattr_settype(&attr, native_type);
    if (!e) e = pthread_mutex_init(m, &attr);
    pthread_mutexattr_destroy(&attr);
    return e;
}
static int native_destroy(NativeMutex *m) { return pthread_mutex_destroy(m); }
static int native_lock(NativeMutex *m) { return pthread_mutex_lock(m); }
static int native_trylock(NativeMutex *m) { return pthread_mutex_trylock(m); }
static struct timespec realtime(uint64_t ns) { return (struct timespec){(time_t)(ns / 1000000000), (long)(ns % 1000000000)}; }
static int native_timedlock(NativeMutex *m, uint64_t deadline) {
    struct timespec end = realtime(deadline);
    return pthread_mutex_timedlock(m, &end);
}
static int native_unlock(NativeMutex *m) { return pthread_mutex_unlock(m); }
static int native_cond_init(NativeCond *c) { return pthread_cond_init(c, NULL); }
static int native_cond_destroy(NativeCond *c) { return pthread_cond_destroy(c); }
static int native_cond_wait(NativeCond *c, NativeMutex *m, uint64_t deadline) {
    if (!deadline) return pthread_cond_wait(c, m);
    struct timespec end = realtime(deadline);
    return pthread_cond_timedwait(c, m, &end);
}
static int native_cond_signal(NativeCond *c) { return pthread_cond_signal(c); }
static int native_cond_broadcast(NativeCond *c) { return pthread_cond_broadcast(c); }
#endif
typedef struct { int type; } GuestAttr;
typedef struct { NativeMutex native; } GuestMutex;
static size_t created, locks, unlocks;
static int32_t orbis_error(int e) {
    if (!e) return 0;
    unsigned code;
    switch (e) {
    case EPERM: code=1; break;
    case ENOMEM: code=12; break;
    case EBUSY: code=16; break;
    case EINVAL: code=22; break;
    case EAGAIN: code=35; break;
    case EDEADLK: code=11; break;
    default: fprintf(stderr, "STOP: unmapped host pthread error %d\n", e); exit(21);
    }
    return (int32_t)(UINT32_C(0x80020000) | code);
}
static ABI int32_t attr_init(GuestAttr **out) {
    if (!out) return orbis_error(EINVAL);
    GuestAttr *attr = malloc(sizeof(*attr));
    if (!attr) return orbis_error(ENOMEM);
    attr->type = 1; *out = attr; return 0;
}
static ABI int32_t attr_type(GuestAttr **attr, int type) {
    if (!attr || !*attr || type < 1 || type > 4) return orbis_error(EINVAL);
    (*attr)->type = type; return 0;
}
/* Priority protocols need a PS4 scheduler; the value is validated and recorded only. */
static ABI int32_t attr_protocol(GuestAttr **attr, int protocol) {
    if (!attr || !*attr || protocol < 0 || protocol > 2) return orbis_error(EINVAL);
    return 0;
}
static ABI int32_t attr_destroy(GuestAttr **attr) {
    if (!attr || !*attr) return orbis_error(EINVAL);
    free(*attr); *attr = NULL; return 0;
}
static ABI int32_t mutex_init(GuestMutex **out, GuestAttr **attr, const char *name) {
    (void)name;
    if (!out || (attr && !*attr)) return orbis_error(EINVAL);
    int type = attr ? (*attr)->type : 1;
    if (type < 1 || type > 4) return orbis_error(EINVAL);
    GuestMutex *mutex = malloc(sizeof(*mutex));
    if (!mutex) return orbis_error(ENOMEM);
    int e = native_init(&mutex->native, type);
    if (e) { free(mutex); return orbis_error(e); }
    *out = mutex; ++created; return 0;
}
static HostMutex static_init = HOST_MUTEX_INIT;
/* Statically initialized handles (0/1) are created once, even under contention. */
static int32_t ensure_mutex(GuestMutex **mutex) {
    if (!mutex) return orbis_error(EINVAL);
    uintptr_t value = __atomic_load_n((uintptr_t *)mutex, __ATOMIC_ACQUIRE);
    if (value == 2) return orbis_error(EINVAL);
    if (value >= 2) return 0;
    host_lock(&static_init);
    int32_t e = 0;
    if ((uintptr_t)*mutex < 2) {
        GuestMutex *created_mutex = NULL;
        e = mutex_init(&created_mutex, NULL, NULL);
        if (!e) __atomic_store_n(mutex, created_mutex, __ATOMIC_RELEASE);
    }
    host_unlock(&static_init);
    return e;
}
static ABI int32_t mutex_lock(GuestMutex **mutex) {
    int32_t e = ensure_mutex(mutex);
    if (e) return e;
    int r = native_trylock(&(*mutex)->native);
    if (r == EBUSY) {
        const uint64_t start = runtime_wait_clock();
        r = native_lock(&(*mutex)->native);
        runtime_wait_note(1, runtime_wait_clock() - start);
    }
    e = orbis_error(r);
    if (!e) ++locks;
    return e;
}
static ABI int32_t mutex_trylock(GuestMutex **mutex) {
    int32_t e = ensure_mutex(mutex);
    if (e) return e;
    e = orbis_error(native_trylock(&(*mutex)->native));
    if (!e) ++locks;
    return e;
}
static ABI int32_t mutex_unlock(GuestMutex **mutex) {
    if (!mutex || (uintptr_t)*mutex == 2) return orbis_error(EINVAL);
    if ((uintptr_t)*mutex < 2) return orbis_error(EPERM);
    int32_t e = orbis_error(native_unlock(&(*mutex)->native));
    if (!e) ++unlocks;
    return e;
}
static ABI int32_t mutex_destroy(GuestMutex **mutex) {
    if (!mutex || (uintptr_t)*mutex == 2) return orbis_error(EINVAL);
    if ((uintptr_t)*mutex < 2) return 0;
    int e = native_destroy(&(*mutex)->native);
    if (!e) { free(*mutex); *mutex = (GuestMutex *)(uintptr_t)2; }
    return orbis_error(e);
}
/* Deadlines are realtime nanoseconds (FreeBSD's default condition variable clock). */
static uint64_t deadline_after(uint64_t usec) { return host_realtime_ns() + usec * 1000; }
static int32_t timed_error(int e) { return e == ETIMEDOUT ? (int32_t)UINT32_C(0x8002003c) : orbis_error(e); }
static ABI int32_t mutex_timedlock(GuestMutex **mutex, uint32_t usec) {
    int32_t e = ensure_mutex(mutex);
    if (e) return e;
    e = timed_error(native_timedlock(&(*mutex)->native, deadline_after(usec)));
    if (!e) ++locks;
    return e;
}
/* Condition variables: opaque handles over the host condition variable (realtime deadlines,
 * like FreeBSD's default), sharing the host mutex behind GuestMutex. */
typedef struct { NativeCond native; } GuestCond;
static size_t conds, waits, wakeups;
static ABI int32_t cond_init(GuestCond **out, void **attr, const char *name) {
    (void)attr; (void)name;
    if (!out) return orbis_error(EINVAL);
    GuestCond *c = malloc(sizeof(*c));
    if (!c) return orbis_error(ENOMEM);
    int e = native_cond_init(&c->native);
    if (e) { free(c); return orbis_error(e); }
    *out = c; ++conds; return 0;
}
static int32_t ensure_cond(GuestCond **cond) {
    if (!cond) return orbis_error(EINVAL);
    if (__atomic_load_n((uintptr_t *)cond, __ATOMIC_ACQUIRE) >= 2) return 0;
    host_lock(&static_init);
    int32_t e = 0;
    if ((uintptr_t)*cond < 2) {
        GuestCond *c = NULL;
        e = cond_init(&c, NULL, NULL);
        if (!e) __atomic_store_n(cond, c, __ATOMIC_RELEASE);
    }
    host_unlock(&static_init);
    return e;
}
static ABI int32_t cond_destroy(GuestCond **cond) {
    if (!cond) return orbis_error(EINVAL);
    if ((uintptr_t)*cond < 2) return 0;
    int e = native_cond_destroy(&(*cond)->native);
    if (!e) { free(*cond); *cond = NULL; }
    return orbis_error(e);
}
static ABI int32_t cond_wait(GuestCond **cond, GuestMutex **mutex) {
    int32_t e = ensure_cond(cond);
    if (e) return e;
    if (!mutex || (uintptr_t)*mutex < 3) return orbis_error(EINVAL);
    ++waits;
    const uint64_t start = runtime_wait_clock();
    const int e2 = native_cond_wait(&(*cond)->native, &(*mutex)->native, 0);
    runtime_wait_note(0, runtime_wait_clock() - start);
    return orbis_error(e2);
}
static int32_t cond_wait_until(GuestCond **cond, GuestMutex **mutex, uint64_t deadline) {
    int32_t e = ensure_cond(cond);
    if (e) return e;
    if (!mutex || (uintptr_t)*mutex < 3) return orbis_error(EINVAL);
    ++waits;
    const uint64_t start = runtime_wait_clock();
    const int e2 = native_cond_wait(&(*cond)->native, &(*mutex)->native, deadline ? deadline : 1);
    runtime_wait_note(0, runtime_wait_clock() - start);
    return timed_error(e2);
}
static ABI int32_t cond_timedwait(GuestCond **cond, GuestMutex **mutex, uint32_t usec) {
    return cond_wait_until(cond, mutex, deadline_after(usec));
}
static ABI int32_t cond_signal(GuestCond **cond) {
    int32_t e = ensure_cond(cond);
    if (e) return e;
    ++wakeups; return orbis_error(native_cond_signal(&(*cond)->native));
}
static ABI int32_t cond_broadcast(GuestCond **cond) {
    int32_t e = ensure_cond(cond);
    if (e) return e;
    ++wakeups; return orbis_error(native_cond_broadcast(&(*cond)->native));
}
/* PS4 struct timespec is {int64 sec, int64 nsec}, identical to Linux x86-64. */
typedef struct { int64_t sec, nsec; } GuestTimespec;
static ABI int32_t cond_abs_timedwait(GuestCond **cond, GuestMutex **mutex, const GuestTimespec *abs) {
    if (!abs || abs->nsec < 0 || abs->nsec >= 1000000000 || abs->sec < 0) return orbis_error(EINVAL);
    return cond_wait_until(cond, mutex, (uint64_t)abs->sec * 1000000000 + (uint64_t)abs->nsec);
}
/* POSIX pthread APIs return positive guest errno values, unlike scePthread. */
static int32_t posix_result(int32_t result) { return result ? (int32_t)((uint32_t)result & 0xffff) : 0; }
static ABI int32_t posix_attr_init(GuestAttr **out) { return posix_result(attr_init(out)); }
static ABI int32_t posix_attr_type(GuestAttr **attr,int type) { return posix_result(attr_type(attr,type)); }
static ABI int32_t posix_attr_destroy(GuestAttr **attr) { return posix_result(attr_destroy(attr)); }
static ABI int32_t posix_mutex_init(GuestMutex **out,GuestAttr **attr) { return posix_result(mutex_init(out,attr,NULL)); }
static ABI int32_t posix_mutex_destroy(GuestMutex **m) { return posix_result(mutex_destroy(m)); }
static ABI int32_t posix_mutex_lock(GuestMutex **m) { return posix_result(mutex_lock(m)); }
static ABI int32_t posix_mutex_trylock(GuestMutex **m) { return posix_result(mutex_trylock(m)); }
static ABI int32_t posix_mutex_unlock(GuestMutex **m) { return posix_result(mutex_unlock(m)); }
static ABI int32_t posix_cond_init(GuestCond **c, void **a) { return posix_result(cond_init(c, a, NULL)); }
static ABI int32_t posix_cond_destroy(GuestCond **c) { return posix_result(cond_destroy(c)); }
static ABI int32_t posix_cond_wait(GuestCond **c, GuestMutex **m) { return posix_result(cond_wait(c, m)); }
static ABI int32_t posix_cond_timedwait(GuestCond **c, GuestMutex **m, const GuestTimespec *t) { return posix_result(cond_abs_timedwait(c, m, t)); }
static ABI int32_t posix_cond_signal(GuestCond **c) { return posix_result(cond_signal(c)); }
static ABI int32_t posix_cond_broadcast(GuestCond **c) { return posix_result(cond_broadcast(c)); }
uintptr_t runtime_mutex_resolve(const char *name) {
    if (!strcmp(name,"0TyVk4MSLt0#I#J")) return (uintptr_t)posix_cond_init;
    if (!strcmp(name,"RXXqi4CtF8w#I#J")) return (uintptr_t)posix_cond_destroy;
    if (!strcmp(name,"Op8TBGY5KHg#I#J")) return (uintptr_t)posix_cond_wait;
    if (!strcmp(name,"27bAgiJmOh0#I#J")) return (uintptr_t)posix_cond_timedwait;
    if (!strcmp(name,"2MOy+rUfuhQ#I#J")) return (uintptr_t)posix_cond_signal;
    if (!strcmp(name,"mkx2fVhNMsg#I#J")) return (uintptr_t)posix_cond_broadcast;
    if (!strcmp(name,"2Tb92quprl0#p#J")) return (uintptr_t)cond_init;
    if (!strcmp(name,"g+PZd2hiacg#p#J")) return (uintptr_t)cond_destroy;
    if (!strcmp(name,"WKAXJ4XBPQ4#p#J")) return (uintptr_t)cond_wait;
    if (!strcmp(name,"BmMjYxmew1w#p#J")) return (uintptr_t)cond_timedwait;
    if (!strcmp(name,"kDh-NfxgMtE#p#J")) return (uintptr_t)cond_signal;
    if (!strcmp(name,"JGgj7Uvrl+A#p#J")) return (uintptr_t)cond_broadcast;
    if (!strcmp(name,"IafI2PxcPnQ#p#J")) return (uintptr_t)mutex_timedlock;
    if (!strcmp(name,"dQHWEsJtoE4#I#J")) return (uintptr_t)posix_attr_init;
    if (!strcmp(name,"mDmgMOGVUqg#I#J")) return (uintptr_t)posix_attr_type;
    if (!strcmp(name,"HF7lK46xzjY#I#J")) return (uintptr_t)posix_attr_destroy;
    if (!strcmp(name,"ttHNfU+qDBU#I#J")) return (uintptr_t)posix_mutex_init;
    if (!strcmp(name,"ltCfaGr2JGE#I#J")) return (uintptr_t)posix_mutex_destroy;
    if (!strcmp(name,"7H0iTOciTLo#I#J")) return (uintptr_t)posix_mutex_lock;
    if (!strcmp(name,"K-jXhbt2gn4#I#J")) return (uintptr_t)posix_mutex_trylock;
    if (!strcmp(name,"2Z+PpY6CaJg#I#J")) return (uintptr_t)posix_mutex_unlock;
    if (!strcmp(name, "F8bUHwAG284#p#J")) return (uintptr_t)attr_init;
    if (!strcmp(name, "iMp8QpE+XO4#p#J")) return (uintptr_t)attr_type;
    if (!strcmp(name, "smWEktiyyG0#p#J")) return (uintptr_t)attr_destroy;
    if (!strcmp(name, "1FGvU0i9saQ#p#J")) return (uintptr_t)attr_protocol;
    if (!strcmp(name, "cmo1RIYva9o#p#J")) return (uintptr_t)mutex_init;
    if (!strcmp(name, "9UK1vLZQft4#p#J")) return (uintptr_t)mutex_lock;
    if (!strcmp(name, "upoVrzMHFeE#p#J")) return (uintptr_t)mutex_trylock;
    if (!strcmp(name, "tn3VlD0hG60#p#J")) return (uintptr_t)mutex_unlock;
    if (!strcmp(name, "2Of0f+3mhhE#p#J")) return (uintptr_t)mutex_destroy;
    return 0;
}
void runtime_mutex_report(void) {
    printf("Runtime: mutexes created=%zu, locks=%zu, unlocks=%zu\n", created, locks, unlocks);
    printf("Runtime: condition variables created=%zu, waits=%zu, wakeups=%zu\n", conds, waits, wakeups);
}
