// SPDX-License-Identifier: GPL-2.0-or-later
// 0.4's optional Linux dma-buf / SIGTRAP experiments cannot run on section-backed Windows
// memory. The established Windows renderer and vectored fault reports remain active.
#ifdef _WIN32
#include <cstdio>
#include <cstdlib>
#include "bbport_guest_memory.h"
#include "bbport_guest_hooks.h"
#include "bbport_gnm_hooks.h"
#include "bbport_free_check.h"
#include "bbport_heap_sites.h"

namespace BbGuestMemory {
bool Usable(const Vulkan::Instance&) { return false; }
bool PcModelGpu(const Vulkan::Instance&) { return false; }
void Install(const Vulkan::Instance&) {
    if (const char* mode = std::getenv("BB_PC_MODEL"); mode && mode[0] == '1')
        std::printf("GPU: experimental Linux memory model unavailable on Windows; using section-backed memory\n");
}
const Chunk* Find(std::uint64_t) { return nullptr; }
}
namespace BbGuestHooks { void Install(RangeCallback) {} }
namespace BbGnmHooks {
void PatchImage(unsigned char*, std::uint64_t) {}
void CheckSubmission(const std::uint32_t*, std::uint64_t) {}
DriverWrite::~DriverWrite() = default;
}
namespace BbFreeCheck {
bool Enabled() { return false; }
void Check(std::uint64_t, std::uint64_t, const void*, Source, std::uint64_t) {}
std::uint64_t NextFenceSeq() { return 0; }
void NoteFenceDecoded(std::uint64_t, std::uint64_t, const void*, const void*, std::uint64_t) {}
void NoteFenceWriting(std::uint64_t) {}
void NoteFenceWritten(std::uint64_t, std::uint64_t) {}
bool OnTrapFault(void*, std::uint64_t) { return false; }
bool OnStaleTrapFault(std::uint64_t) { return false; }
void NoteSubmit(std::uint64_t, const void*, std::uint64_t) {}
void DumpAtFault(std::uint64_t, std::uint64_t) {}
}
namespace BbHeapSites {
void Install() {}
void Report() {}
void NoteReleaseCheck(unsigned) {}
void InstallCounters() {}
bool Counting() { return false; }
long long LiveAllocations() { return 0; }
}
#endif
