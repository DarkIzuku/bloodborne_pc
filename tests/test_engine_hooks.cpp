// SPDX-License-Identifier: GPL-3.0-or-later
// Runs synthetic guest functions through the actual SysV/host bridge, without game data.
#include "gpu/shim/engine/engine_hooks.h"
#include <array>
#include <cassert>
#include <cstring>
#include <cstdio>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

static unsigned calls;
static bool skip;
static std::array<std::uint64_t, 6> arguments;
static double floats[2];

static std::int64_t __attribute__((sysv_abi)) Observe(std::uint64_t, const std::uint64_t* saved) {
    ++calls;
    for (unsigned i = 0; i < 6; ++i) arguments[i] = saved[5 - i];
    std::memcpy(&floats[0], reinterpret_cast<const char*>(saved) - 136, 8);
    std::memcpy(&floats[1], reinterpret_cast<const char*>(saved) - 120, 8);
    // Host code really clobbers these; the displaced guest method must still receive them.
    __asm__ volatile("pxor %%xmm0, %%xmm0\npxor %%xmm1, %%xmm1" ::: "xmm0", "xmm1");
    if (skip) { const_cast<std::uint64_t*>(saved)[-1] = 123; return 1; }
    return 0;
}

int main() {
#ifdef _WIN32
    auto* image = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
#else
    auto* image = static_cast<std::uint8_t*>(mmap(nullptr, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    assert(image != MAP_FAILED);
#endif
    assert(image);
    std::array<std::uint8_t, 16> expected; expected.fill(0x90);
    std::memcpy(image, expected.data(), expected.size());
    // mov rax,rdi; add rax,rsi; add rax,rdx; add rax,rcx; add rax,r8; add rax,r9; ret
    const std::uint8_t sum[] = {0x48,0x89,0xf8,0x48,0x01,0xf0,0x48,0x01,0xd0,0x48,0x01,0xc8,
                               0x4c,0x01,0xc0,0x4c,0x01,0xc8,0xc3};
    std::memcpy(image + 16, sum, sizeof sum);
    BbEngine::Hook hook;
    auto wrong = expected; wrong[0] = 0xcc;
    assert(!BbEngine::Prepare(hook, image, 4096, 0, wrong, Observe));
    assert(!BbEngine::Prepare(hook, image, 8, 0, expected, Observe));
    assert(!std::memcmp(image, expected.data(), expected.size()) && !hook.stub);
    assert(BbEngine::Call(reinterpret_cast<std::uint64_t>(image), 1,2,3,4,5,6) == 21);
    assert(BbEngine::Prepare(hook, image, 4096, 0, expected, Observe));
    // Preparing a complete hook set must not change code before Commit.
    assert(!std::memcmp(image, expected.data(), expected.size()));
    BbEngine::Commit(std::span{&hook, 1});
    assert(BbEngine::Call(reinterpret_cast<std::uint64_t>(image), 1,2,3,4,5,6) == 21);
    assert(calls == 1 && arguments == (std::array<std::uint64_t,6>{1,2,3,4,5,6}));
    skip = true;
    assert(BbEngine::Call(reinterpret_cast<std::uint64_t>(image), 1,2,3,4,5,6) == 123);
    skip = false;
    BbEngine::Discard(hook);

    std::memcpy(image, expected.data(), expected.size());
    const std::uint8_t add_float[] = {0xf2,0x0f,0x58,0xc1,0xc3}; // addsd xmm0,xmm1; ret
    std::memcpy(image + 16, add_float, sizeof add_float);
    assert(BbEngine::Prepare(hook, image, 4096, 0, expected, Observe));
    BbEngine::Commit(std::span{&hook, 1});
    using Floating = double (__attribute__((sysv_abi)) *)(double,double);
    assert(reinterpret_cast<Floating>(image)(1.5,2.5) == 4.0);
    assert(floats[0] == 1.5 && floats[1] == 2.5);
    BbEngine::Discard(hook);
#ifdef _WIN32
    VirtualFree(image, 0, MEM_RELEASE);
#else
    munmap(image, 4096);
#endif
    std::puts("PASS: guest ABI arguments, XMM preservation, return slot, byte/range guards and staged hook installation");
}
