/* host_sync.h: the runtime's threads, clocks and sleeps on each host. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdlib.h>
#ifdef _WIN32
#include <process.h>

/* RuntimeRecoverBuf: rbx rbp rdi rsi r12-r15, rsp after the return, return address, xmm6-xmm15,
 * MXCSR, x87 control word. */
__asm__(".text\n"
        ".globl runtime_setjmp\n"
        ".def runtime_setjmp; .scl 2; .type 32; .endef\n"
        ".p2align 4\n"
        "runtime_setjmp:\n"
        "  movq %rbx, 0(%rcx)\n  movq %rbp, 8(%rcx)\n  movq %rdi, 16(%rcx)\n  movq %rsi, 24(%rcx)\n"
        "  movq %r12, 32(%rcx)\n  movq %r13, 40(%rcx)\n  movq %r14, 48(%rcx)\n  movq %r15, 56(%rcx)\n"
        "  leaq 8(%rsp), %rdx\n  movq %rdx, 64(%rcx)\n  movq (%rsp), %rdx\n  movq %rdx, 72(%rcx)\n"
        "  movups %xmm6, 80(%rcx)\n  movups %xmm7, 96(%rcx)\n  movups %xmm8, 112(%rcx)\n"
        "  movups %xmm9, 128(%rcx)\n  movups %xmm10, 144(%rcx)\n  movups %xmm11, 160(%rcx)\n"
        "  movups %xmm12, 176(%rcx)\n  movups %xmm13, 192(%rcx)\n  movups %xmm14, 208(%rcx)\n"
        "  movups %xmm15, 224(%rcx)\n  stmxcsr 240(%rcx)\n  fnstcw 244(%rcx)\n"
        "  xorl %eax, %eax\n  ret\n"
        ".globl runtime_longjmp\n"
        ".def runtime_longjmp; .scl 2; .type 32; .endef\n"
        ".p2align 4\n"
        "runtime_longjmp:\n"
        "  movq 0(%rcx), %rbx\n  movq 8(%rcx), %rbp\n  movq 16(%rcx), %rdi\n  movq 24(%rcx), %rsi\n"
        "  movq 32(%rcx), %r12\n  movq 40(%rcx), %r13\n  movq 48(%rcx), %r14\n  movq 56(%rcx), %r15\n"
        "  movups 80(%rcx), %xmm6\n  movups 96(%rcx), %xmm7\n  movups 112(%rcx), %xmm8\n"
        "  movups 128(%rcx), %xmm9\n  movups 144(%rcx), %xmm10\n  movups 160(%rcx), %xmm11\n"
        "  movups 176(%rcx), %xmm12\n  movups 192(%rcx), %xmm13\n  movups 208(%rcx), %xmm14\n"
        "  movups 224(%rcx), %xmm15\n  ldmxcsr 240(%rcx)\n  fldcw 244(%rcx)\n"
        "  movq 64(%rcx), %rsp\n  movl $1, %eax\n  jmpq *72(%rcx)\n");

typedef struct { void *(*entry)(void *); void *argument; } Start;
static unsigned __stdcall start(void *p) {
    Start s = *(Start *)p;
    free(p);
    s.entry(s.argument);
    return 0;
}
/* The CRT's thread start (per-thread CRT state). The size is committed up front. The thread
 * starts suspended so *thread is set before it runs (it may detach or join others at once). */
int host_thread_start(HostThread *thread, size_t stack, void *(*entry)(void *), void *argument) {
    Start *s = malloc(sizeof(*s));
    if (!s) return ENOMEM;
    s->entry = entry; s->argument = argument;
    uintptr_t handle = _beginthreadex(NULL, (unsigned)stack, start, s, CREATE_SUSPENDED, NULL);
    if (!handle) { free(s); return EAGAIN; }
    *thread = (HANDLE)handle;
    ResumeThread((HANDLE)handle);
    return 0;
}
int host_thread_join(HostThread thread) {
    DWORD r = WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    return r == WAIT_OBJECT_0 ? 0 : EINVAL;
}
void host_thread_detach(HostThread thread) { CloseHandle(thread); }

static uint64_t qpc_frequency(void) {
    static uint64_t frequency;
    if (!frequency) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); frequency = (uint64_t)f.QuadPart; }
    return frequency;
}
uint64_t host_monotonic_ns(void) {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    uint64_t f = qpc_frequency(), n = (uint64_t)c.QuadPart;
    return n / f * 1000000000u + n % f * 1000000000u / f;
}
uint64_t host_realtime_ns(void) {
    FILETIME t;
    GetSystemTimePreciseAsFileTime(&t);
    uint64_t ticks = ((uint64_t)t.dwHighDateTime << 32) | t.dwLowDateTime; /* 100 ns since 1601 */
    return (ticks - UINT64_C(116444736000000000)) * 100;
}
static __thread HANDLE sleep_timer;
/* High-resolution waitable timers (Windows 10 1803+) wake within ~0.5 ms; the rest of a
 * short wait is spun, so frame pacing and audio deadlines stay precise. */
void host_sleep_until_ns(uint64_t deadline) {
    enum { SPIN_NS = 600000 };
    uint64_t now = host_monotonic_ns();
    if (now >= deadline) return;
    if (deadline - now > SPIN_NS) {
        if (!sleep_timer) {
            sleep_timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
            if (!sleep_timer) sleep_timer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
        }
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)((deadline - now - SPIN_NS) / 100);
        if (sleep_timer && due.QuadPart < 0 && SetWaitableTimer(sleep_timer, &due, 0, NULL, NULL, FALSE))
            WaitForSingleObject(sleep_timer, INFINITE);
        else
            Sleep((DWORD)((deadline - now) / 1000000));
    }
    while (host_monotonic_ns() < deadline) YieldProcessor();
}
void host_sleep_ns(uint64_t ns) {
    if (!ns) { SwitchToThread(); return; }
    host_sleep_until_ns(host_monotonic_ns() + ns);
}
#else
#include <pthread.h>
#include <time.h>

int host_thread_start(HostThread *thread, size_t stack, void *(*entry)(void *), void *argument) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (stack) pthread_attr_setstacksize(&attr, stack);
    int e = pthread_create(thread, &attr, entry, argument);
    pthread_attr_destroy(&attr);
    return e;
}
int host_thread_join(HostThread thread) { return pthread_join(thread, NULL); }
void host_thread_detach(HostThread thread) { pthread_detach(thread); }
static uint64_t read_clock(clockid_t id) {
    struct timespec t;
    clock_gettime(id, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}
uint64_t host_monotonic_ns(void) { return read_clock(CLOCK_MONOTONIC); }
uint64_t host_realtime_ns(void) { return read_clock(CLOCK_REALTIME); }
void host_sleep_ns(uint64_t ns) {
    struct timespec t = {.tv_sec = (time_t)(ns / 1000000000), .tv_nsec = (long)(ns % 1000000000)};
    while (nanosleep(&t, &t) && errno == EINTR) {}
}
void host_sleep_until_ns(uint64_t deadline) {
    struct timespec t = {(time_t)(deadline / 1000000000u), (long)(deadline % 1000000000u)};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL)) {}
}
#endif
