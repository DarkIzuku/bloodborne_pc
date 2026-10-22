// SPDX-License-Identifier: GPL-2.0-or-later
// Run the actual nested PM4 decoders without a GPU; track outstanding C++ allocations.
#include <array>
#include <cstdlib>
#include <cstdio>
#include <new>
#include "video_core/amdgpu/liverpool.h"
#include "video_core/amdgpu/pm4_cmds.h"
#include "test_assert.h"

namespace {
thread_local bool tracking=false;
thread_local std::array<void*,4096> live{};
thread_local unsigned count=0;
void* Allocate(std::size_t bytes) {
    void* p=std::malloc(bytes ? bytes : 1);
    if(!p) throw std::bad_alloc{};
    if(tracking) {
        bool stored=false;
        for(auto& entry:live) if(!entry) { entry=p;stored=true;++count;break; }
        assert(stored);
    }
    return p;
}
void Free(void* p) noexcept {
    if(p) for(auto& entry:live) if(entry==p) {entry=nullptr;--count;break;}
    std::free(p);
}
}
void* operator new(std::size_t bytes) {return Allocate(bytes);}
void* operator new[](std::size_t bytes) {return Allocate(bytes);}
void operator delete(void* p) noexcept {Free(p);}
void operator delete[](void* p) noexcept {Free(p);}
void operator delete(void* p,std::size_t) noexcept {Free(p);}
void operator delete[](void* p,std::size_t) noexcept {Free(p);}

namespace AmdGpu {
struct LiverpoolTaskTest {
    static void Run() {
        // Constructor's idle decoder thread never receives work. All test decodes are
        // driven on this thread, so allocation tracking cannot count background work.
        Liverpool cpu;
        u32 read=0;
        const auto queue=cpu.asc_queues.insert();
        assert(queue.index==0);
        cpu.asc_queues[queue].read_addr=&read;
        cpu.asc_queues[queue].ring_size_dw=4096;
        constexpr unsigned depth=12;
        std::array<std::array<u32,4>,depth> gfx{},ce{},compute{};
        const std::array<u32,2> leaf={PM4Type3Header(PM4ItOpcode::Nop,0).raw,0};
        const auto build=[&](auto& chain,PM4ItOpcode opcode) {
            std::span<const u32> next=leaf;
            for(auto& words:chain) {
                const auto address=reinterpret_cast<uintptr_t>(next.data());
                words={PM4Type3Header(opcode,2).raw,u32(address),u32(address>>32),u32(next.size())};
                next=words;
            }
        };
        build(gfx,PM4ItOpcode::IndirectBuffer);
        build(ce,PM4ItOpcode::IndirectBufferConst);
        build(compute,PM4ItOpcode::IndirectBuffer);
        const auto drain=[](Liverpool::Task task) {
            unsigned resumes=0;
            while(!task.handle.done()) {assert(++resumes<1024);task.handle.resume();}
            task.handle.destroy();
        };
        // Warm static diagnostic flags and shared services before counting frames.
        drain(cpu.ProcessGraphics(gfx.back(),ce.back()));
        drain(cpu.ProcessCompute<true>(compute.back(),0));
        for(unsigned pass=0;pass<2000;++pass) {
            tracking=true;
            drain(cpu.ProcessGraphics(gfx.back(),ce.back()));
            drain(cpu.ProcessCompute<true>(compute.back(),0));
            tracking=false;
            assert(count==0); // Fails immediately if any nested frame survives final_suspend.
        }
    }
};
}
int main() {
    AmdGpu::LiverpoolTaskTest::Run();
    std::puts("PASS: 2000 nested graphics/constant/compute decodes, zero outstanding allocations");
}
