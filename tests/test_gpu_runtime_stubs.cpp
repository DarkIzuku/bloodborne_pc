// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdlib>
#include <chrono>
#include <cstdint>
#include <cstddef>
#include <setjmp.h>
#ifdef _WIN32
struct alignas(16) RuntimeRecoverBuf {
    unsigned char registers[256];
};
#else
typedef sigjmp_buf RuntimeRecoverBuf;
#endif
// Renderer tests have no guest process. Clock/host-thread services work; guest accesses abort.
extern "C" {
// Engine asset mounts belong to the runtime; GPU-only tests never install them.
int runtime_file_mount(const char*, const char*) { return -1; }
void runtime_file_unmount(const char*) {}
__thread RuntimeRecoverBuf* runtime_fault_recover = nullptr;
int runtime_setjmp(RuntimeRecoverBuf*) { return 0; }
[[noreturn]] void runtime_longjmp(RuntimeRecoverBuf*) { std::abort(); }
uint32_t runtime_disabled_optimizations = 0;
uint64_t runtime_experiment_bits = 0;
uint64_t runtime_tsc_frequency() { return 1000000000; }
int runtime_file_translate(const char*, char*, size_t) { std::abort(); }
uint64_t runtime_memory_clamp(uintptr_t, uint64_t) { std::abort(); }
uint64_t runtime_process_time_us() { return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
int runtime_memory_region(uintptr_t, uintptr_t*, uintptr_t*, int*) { std::abort(); }
void runtime_memory_set_gpu_hooks(void (*)(uintptr_t, uint64_t),
    void (*)(uintptr_t, uint64_t), void (*)(uintptr_t, uint64_t)) { std::abort(); }
void runtime_thread_attach_host(const char*) {}
void runtime_memory_gpu_protect(uintptr_t, uint64_t, int, int) { std::abort(); }
void runtime_restart() { std::abort(); }
uint64_t runtime_process_time_counter() { return runtime_process_time_us() * 1000; }
int32_t* runtime_errno() { std::abort(); }
int runtime_memory_write_backing(uintptr_t, const void*, uint64_t) { std::abort(); }
void runtime_memory_read_backing(uintptr_t, void*, uint64_t) { std::abort(); }
const uint64_t* runtime_memory_generation() { static const uint64_t generation = 0; return &generation; }
void runtime_memory_note_write(uintptr_t, uint64_t) { std::abort(); }
void runtime_memory_set_note_write_hook(void (*)(uintptr_t, uint64_t)) {}
void runtime_memory_set_cpu_write_hook(void (*)(uintptr_t, uint64_t)) {}
void runtime_memory_set_write_watch(uintptr_t, uint64_t, int) { std::abort(); }
void runtime_memory_set_guest_chunk_allocator(int (*)(uint64_t, uint64_t)) {}
void runtime_memory_set_guest_chunk_whole(int) {}
int runtime_memory_vma_info(uintptr_t, int*, int*, uintptr_t*) { std::abort(); }
int runtime_memory_direct_phys(uintptr_t, uint64_t*, uintptr_t*) { std::abort(); }
void runtime_wait_report(double) {}
void runtime_sleep_stats(uint64_t* calls, uint64_t* ns) { *calls = 0; *ns = 0; }
uint64_t runtime_heap_growths() { return 0; }
}

