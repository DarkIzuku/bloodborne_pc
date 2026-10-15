// SPDX-License-Identifier: GPL-3.0-or-later
#include "frame_pacing.h"
#include "core/emulator_settings.h"
#include "test_assert.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

namespace BbPlatform {
bool ReadProcessMemory(const void* p,void* out,std::size_t size) {
#ifdef _WIN32
    SIZE_T read=0;
    return ::ReadProcessMemory(GetCurrentProcess(),p,out,size,&read) && read==size;
#else
    if(!p) return false;
    std::memcpy(out,p,size);return true;
#endif
}
}
int main() {
    using namespace BbFramePacing;
    using namespace std::chrono;
    for(unsigned cap:{0u,30u,60u,90u,120u}) {
        assert(Period(cap,Scene::FrontEnd,false,240).count()>=16666667);
        assert(Period(cap,Scene::Loading,false,240).count()>=16666667);
        assert(Period(cap,Scene::Gameplay,false,60).count()==(cap?std::int64_t(std::ceil(1e9/cap)):0));
        assert(Period(cap,Scene::Gameplay,true,59.94).count()>=16683351);
    }
    assert(Period(0,Scene::Gameplay,true,144).count()==6944445);
    assert(Period(120,Scene::Gameplay,true,240).count()==8333334);
    assert(Period(0,Scene::Gameplay,true,std::numeric_limits<double>::quiet_NaN()).count()==16666667);
    Limiter l;
    const Limiter::Time zero{};
    auto tick=nanoseconds(16666667);
    l.Configure(tick,zero);
    assert(l.Due(zero));l.Presented(zero);
    assert(!l.Due(zero+tick-1ns) && l.Due(zero+tick));
    l.Presented(zero+10s);assert(!l.Due(zero+10s+1ns)); // no catch-up burst
    l.Configure(0ns,zero+11s);assert(l.Due(zero+11s));
    l.Presented(zero+11s);
    l.Configure(tick,zero+11s+1ms);assert(!l.Due(zero+11s+1ms));
    for(int i=0;i<100000;i++) {auto now=zero+12s+tick*i;assert(l.Due(now));l.Presented(now);assert(!l.Due(now+1ns));}
    std::puts("PASS: limits, fractional VSync, no catch-up and 100000 paced frames");

    // Windows can wake a little after every deadline. Advancing from each wake
    // instead of the scheduled slot turns a 60 FPS cap into about 57 FPS.
    for (double fps : {30.0, 60.0, 90.0, 120.0, 143.856}) {
        const auto period = Period(0, Scene::Gameplay, true, fps);
        Limiter paced;
        paced.Configure(period, zero);
        paced.Presented(zero);
        for (int frame = 1; frame <= 10000; ++frame) {
            const auto wake_delay = (frame % 3 == 0 ? 1300us : 900us);
            const auto scheduled = zero + period * frame;
            const auto wake = scheduled + wake_delay;
            assert(paced.Next() == scheduled);
            assert(!paced.Due(scheduled - 1ns) && paced.Due(wake));
            paced.Presented(wake);
            assert(paced.Next() == scheduled + period);
            assert(!paced.Due(wake));
        }
        // A stall must discard timing debt rather than emit catch-up frames.
        const auto late = paced.Next() + 250ms;
        paced.Presented(late);
        assert(paced.Next() == late + period);
        assert(!paced.Due(late + period - 1ns));
        const auto half_frame_late = paced.Next() + period / 2;
        paced.Presented(half_frame_late);
        assert(paced.Next() == half_frame_late + period);
    }
    std::puts("PASS: wake overshoot preserves nominal FPS; stalls rephase without catch-up");

    // A producer may wait for one vblank before submitting every frame. Both
    // clocks need a stable phase; fixing only presentation still leaves this
    // producer limited by the drifting event cadence.
    Limiter vblank, output;
    vblank.Configure(tick, zero);
    output.Configure(tick, zero);
    vblank.Presented(zero);
    output.Presented(zero);
    for (int frame = 1; frame <= 10000; ++frame) {
        const auto wake = vblank.Next() + 900us;
        vblank.Presented(wake);
        const auto present = wake + 100us;
        assert(output.Due(present));
        output.Presented(present);
        assert(vblank.Next() == zero + tick * (frame + 1));
        assert(output.Next() == vblank.Next());
    }
    std::puts("PASS: vblank-driven frame producer retains nominal 60 FPS cadence");

    std::vector<std::uint8_t> image(0x5580e40);
    std::array<std::uint8_t,25> bytes={0x48,0x8b,0x05,0xd1,0xe1,0x9c,0x03,0x48,0x85,0xc0,0x74,0x0b,0x80,0xb8,0x11,0x0b,0x00,0x00,0x00,0x0f,0x94,0xc0,0xc3,0x31,0xc0};
    std::memcpy(image.data()+0x1bb2c60,bytes.data(),bytes.size());
    const char* hash="071df19c8880086d97182dbc057bc8cb37badaca57d9112683836b24a0444c0a";
    Initialize(image.data(),image.size(),"wrong");assert(CurrentScene()==Scene::FrontEnd);
    Initialize(image.data(),image.size(),hash);assert(CurrentScene()==Scene::FrontEnd);
    std::array<std::uint8_t,0xb20> flow{};
    std::array<std::uint8_t,0x68> world{};
    std::array<std::uint8_t,0x400> player{};
    auto put=[](auto* at,const auto& value){std::memcpy(at,&value,sizeof(value));};
    auto ptr=reinterpret_cast<std::uintptr_t>(flow.data());put(image.data()+0x5580e38,ptr);
    assert(CurrentScene()==Scene::FrontEnd);
    flow[0xb11]=1;assert(CurrentScene()==Scene::Loading);
    ptr=reinterpret_cast<std::uintptr_t>(world.data());put(image.data()+0x553e878,ptr);
    ptr=reinterpret_cast<std::uintptr_t>(player.data());put(world.data()+0x60,ptr);
    assert(CurrentScene()==Scene::Gameplay);
    // Inventory and other in-world menus live at +0xa88, not +0xb11.
    std::uint64_t open_menu=1234;put(flow.data()+0xa88,open_menu);
    assert(CurrentScene()==Scene::Gameplay);
    SetVSync(false);assert(CurrentPeriod(0)==0ns);
    SetDisplayRefresh(143.856);SetVSync(true);
    assert(CurrentPeriod(0).count()==6951396);
    flow[0xb11]=0;assert(CurrentScene()==Scene::FrontEnd);
    assert(CurrentPeriod(0).count()==16666667);
    image[0x1bb2c60]^=1;
    Initialize(image.data(),image.size(),hash);assert(CurrentScene()==Scene::FrontEnd);
    Initialize(image.data(),32,hash);assert(CurrentScene()==Scene::FrontEnd);
    std::puts("PASS: verified title state, loading, inventory exclusion, return to title and safe mismatch");
}
