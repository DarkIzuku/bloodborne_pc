#include "video_core/texture_cache/gc_policy.h"
#include <cassert>
#include <cstdint>
#include <limits>
int main() {
    using namespace VideoCore::GcPolicy;
    assert(Pressure(100,0)==100);
    assert(Pressure(100,50)==100);
    assert(Pressure(100,1000)==800);
    assert(Pressure(100,1001)==800);
    const auto max=std::numeric_limits<std::uint64_t>::max();
    assert(Pressure(0,max)<max && Pressure(0,max)>max/2);
    assert(!CompletedUse(20,20,true)); // not submitted yet, even if a caller reports completion
    assert(!CompletedUse(19,20,false)); // actually in flight
    assert(!CompletedUse(21,20,true)); // future/reused tick
    assert(CompletedUse(19,20,true));
    assert(CompletedUse(0,1,true));
    assert(FullMip(64,32,2,4,16384));
    assert(!FullMip(64,32,2,4,16383));
    assert(!FullMip(0,32,2,4,16384));
    assert(!FullMip(UINT32_MAX,UINT32_MAX,UINT32_MAX,16,max));
    assert(WritebackMipChain(false,2));
    assert(WritebackMipChain(true,1));
    assert(!WritebackMipChain(true,2));
    assert(!WritebackMipChain(false,0));
    assert(!WritebackMipChain(false,17));
}
