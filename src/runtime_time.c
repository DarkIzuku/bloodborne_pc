/* Explicit guest timeval layout; host pointers never escape this boundary. */
#define _DEFAULT_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef _WIN32
#include <sys/time.h>
#endif
typedef struct { int64_t seconds, microseconds; } GuestTimeval;
typedef struct { int32_t minuteswest, dsttime; } GuestTimezone;
static ABI int guest_gettimeofday(GuestTimeval *value,GuestTimezone *zone) {
#ifdef _WIN32
    uint64_t now=host_realtime_ns();
    if (value) { value->seconds=(int64_t)(now/1000000000); value->microseconds=(int64_t)(now%1000000000/1000); }
    if (zone) { zone->minuteswest=(int32_t)(-runtime_utc_offset(time(NULL))/60); zone->dsttime=0; }
#else
    struct timeval t;
    struct timezone z;
    if (gettimeofday(&t,&z)) {
        /* Guest errno/TLS failure path is not implemented yet. */
        perror("STOP: host gettimeofday failed"); exit(21);
    }
    if (value) { value->seconds=t.tv_sec; value->microseconds=t.tv_usec; }
    if (zone) { zone->minuteswest=z.tz_minuteswest; zone->dsttime=z.tz_dsttime; }
#endif
    return 0;
}
uintptr_t runtime_time_resolve(const char *name) {
    if (!strcmp(name,"n88vx3C5nW8#I#J")) return (uintptr_t)guest_gettimeofday;
    return 0;
}
