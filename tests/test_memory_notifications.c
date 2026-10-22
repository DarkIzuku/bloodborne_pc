/* SPDX-License-Identifier: GPL-3.0-or-later
 * Exercises the real runtime queue and Windows section remapping, without a game. */
#include "../src/runtime_memory.c"
#include <assert.h>

/* No guest ABI dispatch or toggle watcher participates in these tests. If one
 * accidentally becomes necessary, fail instead of returning a fake success. */
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *nid) {
    (void)table; (void)count; (void)nid; abort();
}
int32_t *runtime_errno(void) { abort(); }
void runtime_guest_call_sites(uint64_t out[3]) { (void)out; abort(); }
int host_thread_start(HostThread *thread, size_t stack, void *(*entry)(void*), void *argument) {
    (void)thread; (void)stack; (void)entry; (void)argument; abort();
}
void host_sleep_ns(uint64_t ns) { (void)ns; abort(); }

static size_t seen, invalidations;
static uintptr_t expected;
static void ordered(uintptr_t address, uint64_t size) {
    assert(address==expected++ && size==4);
    ++seen;
}
static void ignore(uintptr_t address, uint64_t size) { (void)address; (void)size; }
static void invalidate(uintptr_t address, uint64_t size) {
    assert(address && size); ++invalidations;
}
int main(void) {
    hook_map=ordered;
    write_lock();
    for (uintptr_t i=0;i<4097;++i) queue_hook(HOOK_MAP,i,4);
    write_unlock();
    flush_hooks();
    assert(seen==4097 && pending_count==0);
    // Recycle the allocated queue without losing order or retaining callbacks.
    write_lock();
    for (uintptr_t i=4097;i<8194;++i) queue_hook(HOOK_MAP,i,4);
    write_unlock();
    flush_hooks();
    assert(seen==8194 && pending_count==0);

    hook_map=ignore; hook_unmap=ignore; hook_invalidate=invalidate;
    // This fixture needs only three views, not a reservation of the entire
    // guest VA range (which can contain DLL mappings in a test runner).
    assert(win_mem_space(USER_MIN,USER_MIN+64*UINT64_C(1024)*1024)==0);
    assert(_putenv_s("BB_DMEM_MB","512")==0);
    const uint64_t length=3*PAGE;
    int64_t physical=-1;
    void *mapped=NULL;
    assert(direct_allocate(0,POOL_SIZE,length,PAGE,0,&physical)==0);
    assert(direct_map(&mapped,length,3,0,physical,PAGE)==0);
    MEMORY_BASIC_INFORMATION state;
    assert(VirtualQuery(backing_base+physical,&state,sizeof(state)) && state.State==MEM_COMMIT);
    assert(VirtualQuery(backing_base+physical+length,&state,sizeof(state)) && state.State==MEM_RESERVE);
    assert(win_mem_commit(backing_base+POOL_SIZE+FLEX_SPAN,PAGE)==-1);
    memset(mapped,0xa5,length);
    void *alias=NULL;
    assert(direct_map(&alias,length,3,0,physical,PAGE)==0);
    assert(((unsigned char*)alias)[length-1]==0xa5);
    ((unsigned char*)alias)[0]=0x5a;
    assert(((unsigned char*)mapped)[0]==0x5a);
    assert(direct_unmap(alias,length)==0);
    invalidations=0;
    // Removing the middle of a Windows section remaps the surviving pieces.
    // Their INVALIDATE notifications used to be discarded together with UNMAP.
    assert(direct_unmap((char*)mapped+PAGE,PAGE)==0);
#ifdef _WIN32
    assert(invalidations>=2);
#endif
    assert(pending_count==0);
    assert(direct_release(physical,length)==0);
    void *flex=NULL;
    assert(map_flexible(&flex,PAGE,3,0)==0);
    memset(flex,0x42,PAGE);
    assert(((unsigned char*)flex)[PAGE-1]==0x42);
    assert(direct_unmap(flex,PAGE)==0);
    assert(direct_allocate(0,POOL_SIZE,length,PAGE,0,&physical)==0);
    assert(backing_base[physical]==0 && backing_base[physical+length-1]==0);
    assert(direct_release(physical,length)==0);
    free(pending); pending=NULL; pending_capacity=0;
    puts("PASS: 8194 ordered notifications, queue reuse and surviving-view invalidations");
}
