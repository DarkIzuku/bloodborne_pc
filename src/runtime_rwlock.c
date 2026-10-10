/* Host rwlocks (pthread rwlocks on Linux, SRW locks on Windows) behind the PS4
   pointer-to-handle ABI. The registry protects lifetime, static initialization and
   ownership; waiting happens outside it. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
typedef struct { int64_t seconds, nanoseconds; } GuestTime;
#ifdef _WIN32
typedef SRWLOCK NativeRwlock;
static int native_init(NativeRwlock *l) { InitializeSRWLock(l); return 0; }
static int native_destroy(NativeRwlock *l) { (void)l; return 0; }
static int native_try(NativeRwlock *l, int writer) {
    return (writer ? TryAcquireSRWLockExclusive(l) : TryAcquireSRWLockShared(l)) ? 0 : EBUSY;
}
static int native_lock(NativeRwlock *l, int writer) {
    if (writer) AcquireSRWLockExclusive(l); else AcquireSRWLockShared(l);
    return 0;
}
/* SRW locks have no timed acquire: retry until the realtime deadline. */
static int native_timedlock(NativeRwlock *l, int writer, const GuestTime *time) {
    uint64_t deadline=(uint64_t)time->seconds*1000000000+(uint64_t)time->nanoseconds;
    for (unsigned attempt=0;;++attempt) {
        if (!native_try(l,writer)) return 0;
        if (host_realtime_ns()>=deadline) return ETIMEDOUT;
        if (attempt<64) YieldProcessor(); else if (attempt<128) SwitchToThread(); else Sleep(1);
    }
}
static int native_unlock(NativeRwlock *l, int writer) {
    if (writer) ReleaseSRWLockExclusive(l); else ReleaseSRWLockShared(l);
    return 0;
}
#else
typedef pthread_rwlock_t NativeRwlock;
static int native_init(NativeRwlock *l) { return pthread_rwlock_init(l,NULL); }
static int native_destroy(NativeRwlock *l) { return pthread_rwlock_destroy(l); }
static int native_try(NativeRwlock *l, int writer) { return writer ? pthread_rwlock_trywrlock(l) : pthread_rwlock_tryrdlock(l); }
static int native_lock(NativeRwlock *l, int writer) { return writer ? pthread_rwlock_wrlock(l) : pthread_rwlock_rdlock(l); }
static int native_timedlock(NativeRwlock *l, int writer, const GuestTime *time) {
    struct timespec deadline={.tv_sec=(time_t)time->seconds,.tv_nsec=(long)time->nanoseconds};
    return writer ? pthread_rwlock_timedwrlock(l,&deadline) : pthread_rwlock_timedrdlock(l,&deadline);
}
static int native_unlock(NativeRwlock *l, int writer) { (void)writer; return pthread_rwlock_unlock(l); }
#endif
typedef struct Holder {
    HostThreadId thread;
    unsigned readers, writer, pending;
    struct Holder *next;
} Holder;
typedef struct Rwlock {
    NativeRwlock native;
    Holder *holders;
    unsigned inflight;
    struct Rwlock *next;
} Rwlock;
static HostMutex registry_lock = HOST_MUTEX_INIT;
static Rwlock *registry;
static size_t created, reads, writes, unlocks;
static int32_t error(int e) {
    if (!e) return 0;
    unsigned code;
    switch(e) {
    case EPERM: code=1; break;
    case EDEADLK: code=11; break;
    case ENOMEM: code=12; break;
    case EBUSY: code=16; break;
    case EINVAL: code=22; break;
    case EAGAIN: code=35; break;
    case ETIMEDOUT: code=60; break;
    default: fprintf(stderr,"STOP: unmapped rwlock error %d\n",e); exit(21);
    }
    return (int32_t)(UINT32_C(0x80020000)|code);
}
/* All helpers ending in _locked require registry_lock. */
static Rwlock *find_locked(Rwlock *handle) {
    for (Rwlock *r=registry; r; r=r->next) if (r==handle) return r;
    return NULL;
}
static int create_locked(Rwlock **out) {
    Rwlock *r=calloc(1,sizeof(*r));
    if (!r) return ENOMEM;
    int e=native_init(&r->native);
    if (e) { free(r); return e; }
    r->next=registry; registry=r; *out=r; ++created; return 0;
}
static ABI int32_t rw_init(Rwlock **out, void *const *attr, const char *name) {
    (void)name;
    if (!out || (attr && !*attr)) return error(EINVAL);
    if (attr) { fputs("STOP: non-default PS4 rwlock attributes are not implemented\n",stderr); exit(21); }
    host_lock(&registry_lock);
    /* POSIX allows uninitialized storage, so never read *out during init. */
    int e=create_locked(out);
    host_unlock(&registry_lock); return error(e);
}
static void prune_locked(Rwlock *r, Holder *h) {
    if (h->readers || h->writer || h->pending) return;
    Holder **link=&r->holders;
    while (*link!=h) link=&(*link)->next;
    *link=h->next; free(h);
}
enum Operation { READ, WRITE, TRY_READ, TRY_WRITE, TIMED_READ, TIMED_WRITE };
static int32_t acquire(Rwlock **handle, enum Operation op, const GuestTime *time) {
    if (!handle) return error(EINVAL);
    int writer=op==WRITE || op==TRY_WRITE || op==TIMED_WRITE;
    int attempt=op==TRY_READ || op==TRY_WRITE;
    int timed=op==TIMED_READ || op==TIMED_WRITE;
    host_lock(&registry_lock);
    int e=0;
    if (!*handle) e=create_locked(handle);
    Rwlock *r=e ? NULL : find_locked(*handle);
    if (!r) { host_unlock(&registry_lock); return error(e ? e : EINVAL); }
    Holder *h=r->holders;
    while (h && !host_thread_equal(h->thread,host_thread_id())) h=h->next;
    if (h && (h->writer || (writer && h->readers))) {
        host_unlock(&registry_lock); return error(attempt ? EBUSY : EDEADLK);
    }
    if (h && h->readers==UINT_MAX) { host_unlock(&registry_lock); return error(EAGAIN); }
    if (!h) {
        h=calloc(1,sizeof(*h));
        if (!h) { host_unlock(&registry_lock); return error(ENOMEM); }
        h->thread=host_thread_id(); h->next=r->holders; r->holders=h;
    }
    ++r->inflight; ++h->pending;
    host_unlock(&registry_lock);
    /* Timed operations first try the lock, matching POSIX timeout validation. */
    if (attempt || timed) e=native_try(&r->native,writer);
    else e=native_lock(&r->native,writer);
    if (timed && e==EBUSY) {
        if (!time || time->nanoseconds<0 || time->nanoseconds>=1000000000 || time->seconds<0) e=EINVAL;
        else e=native_timedlock(&r->native,writer,time);
    }
    host_lock(&registry_lock);
    --r->inflight; --h->pending;
    if (!e) {
        if (writer) { h->writer=1; ++writes; }
        else { ++h->readers; ++reads; }
    }
    prune_locked(r,h);
    host_unlock(&registry_lock); return error(e);
}
static ABI int32_t rw_read(Rwlock **r) { return acquire(r,READ,NULL); }
static ABI int32_t rw_write(Rwlock **r) { return acquire(r,WRITE,NULL); }
static ABI int32_t rw_tryread(Rwlock **r) { return acquire(r,TRY_READ,NULL); }
static ABI int32_t rw_trywrite(Rwlock **r) { return acquire(r,TRY_WRITE,NULL); }
static ABI int32_t rw_timedread(Rwlock **r,const GuestTime *t) { return acquire(r,TIMED_READ,t); }
static ABI int32_t rw_timedwrite(Rwlock **r,const GuestTime *t) { return acquire(r,TIMED_WRITE,t); }
static ABI int32_t rw_unlock(Rwlock **handle) {
    if (!handle) return error(EINVAL);
    host_lock(&registry_lock);
    Rwlock *r=find_locked(*handle);
    if (!r) { host_unlock(&registry_lock); return error(EINVAL); }
    Holder *h=r->holders;
    while (h && !host_thread_equal(h->thread,host_thread_id())) h=h->next;
    if (!h || (!h->readers && !h->writer)) { host_unlock(&registry_lock); return error(EPERM); }
    int e=native_unlock(&r->native,h->writer);
    if (!e) { if (h->writer) h->writer=0; else --h->readers; ++unlocks; }
    prune_locked(r,h);
    host_unlock(&registry_lock); return error(e);
}
static ABI int32_t rw_destroy(Rwlock **handle) {
    if (!handle) return error(EINVAL);
    host_lock(&registry_lock);
    if (!*handle) { host_unlock(&registry_lock); return 0; }
    Rwlock *r=find_locked(*handle);
    if (!r) { host_unlock(&registry_lock); return error(EINVAL); }
    if (r->inflight || r->holders) { host_unlock(&registry_lock); return error(EBUSY); }
    int e=native_destroy(&r->native);
    if (!e) {
        Rwlock **link=&registry;
        while (*link!=r) link=&(*link)->next;
        *link=r->next; free(r); *handle=(Rwlock *)(uintptr_t)1;
    }
    host_unlock(&registry_lock); return error(e);
}
uintptr_t runtime_rwlock_resolve(const char *name) {
    if (!strcmp(name,"6ULAa0fq4jA#p#J")) return (uintptr_t)rw_init;
    if (!strcmp(name,"BB+kb08Tl9A#p#J")) return (uintptr_t)rw_destroy;
    if (!strcmp(name,"Ox9i0c7L5w0#p#J")) return (uintptr_t)rw_read;
    if (!strcmp(name,"mqdNorrB+gI#p#J")) return (uintptr_t)rw_write;
    if (!strcmp(name,"XD3mDeybCnk#p#J")) return (uintptr_t)rw_tryread;
    if (!strcmp(name,"bIHoZCTomsI#p#J")) return (uintptr_t)rw_trywrite;
    if (!strcmp(name,"+L98PIbGttk#p#J")) return (uintptr_t)rw_unlock;
    if (!strcmp(name,"iPtZRWICjrM#p#J")) return (uintptr_t)rw_timedread;
    if (!strcmp(name,"adh--6nIqTk#p#J")) return (uintptr_t)rw_timedwrite;
    return 0;
}
void runtime_rwlock_report(void) {
    host_lock(&registry_lock);
    printf("Runtime: rwlocks created=%zu, reads=%zu, writes=%zu, unlocks=%zu\n",created,reads,writes,unlocks);
    host_unlock(&registry_lock);
}
