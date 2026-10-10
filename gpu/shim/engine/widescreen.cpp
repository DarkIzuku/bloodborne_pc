// SPDX-License-Identifier: GPL-3.0-or-later
// Camera blend, stage pins and projected plates adapted from bbhost db5457cb.
// Existing bbport resolution, Vulkan targets and temporal providers remain authoritative.
#include "widescreen.h"
#include "engine_hooks.h"
#include "bbport_settings.h"
#include "bbport_platform.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace BbEngine::Widescreen {
namespace {
std::uint8_t* base;
std::size_t image_size;
bool installed;
constexpr float original_aspect=16.0f/9.0f;
std::atomic<float> aspect{original_aspect},previous_aspect{original_aspect};
struct Stage { std::size_t at,reads;std::uint32_t value;bool cx=false; };
constexpr Stage stage[]={
    {0x15e83af,0x51289f8,1920},{0x1bfd491,0x51289f8,1920},{0x1bfd4d1,0x51289fc,1080},
    {0x1644357,0x51289f8,1920},{0x1644365,0x51289fc,1080},{0x1644c55,0x51289f8,1920},
    {0x1644c63,0x51289fc,1080},{0x16452c7,0x51289f8,1920},{0x16452d5,0x51289fc,1080},
    {0x1654df6,0x51289f8,1920},{0x1654e1c,0x51289fc,1080},{0x1655611,0x51289fc,1080},
    {0x1655628,0x51289f8,1920},{0x16b3372,0x51289f8,1920,true},{0x16b3398,0x51289fc,1080}};
constexpr std::size_t plates[]={0x48f9a00,0x48f9a60,0x48f9ad0};
constexpr std::size_t right_bounds[]={0x1bfd491,0x1654df6,0x16b3372};
constexpr std::size_t bottom_bounds[]={0x1bfd4d1,0x1654e1c,0x16b3398};
struct Branch {std::size_t at;std::array<std::uint8_t,3> before,after;std::size_t n;};
constexpr Branch left[]={
    {0x1bfd473,{0x77,0x30},{0x66,0x90},2},{0x1654dd9,{0x77,0x2f},{0x66,0x90},2},
    {0x16b3370,{0x77,0x5a},{0x66,0x90},2},{0x1642520,{0x0f,0x96,0xc1},{0xb1,0x01,0x90},3}};
constexpr Branch top[]={
    {0x1bfd4cf,{0x77,0x14},{0x66,0x90},2},{0x1654e1a,{0x77,0x14},{0x66,0x90},2},
    {0x16b3396,{0x77,0x34},{0x66,0x90},2},{0x164253c,{0x77,0x20},{0x66,0x90},2}};
template<typename T> bool Read(std::uint64_t address,T& value) {
    return address && BbPlatform::ReadProcessMemory(reinterpret_cast<void*>(address),&value,sizeof value);
}
bool Matches(std::size_t at,const void* bytes,std::size_t n) {
    return at<=image_size && n<=image_size-at && !std::memcmp(base+at,bytes,n);
}
std::int64_t __attribute__((sysv_abi)) Blend(std::uint64_t,const std::uint64_t* saved) {
    auto fix=[](std::uint64_t camera) {
        float a=0;const float want=aspect.load(),previous=previous_aspect.load();
        if (Read(camera+0x54,a) && a!=want && (std::fabs(a-original_aspect)<1e-4f || a==previous))
            std::memcpy(reinterpret_cast<void*>(camera+0x54),&want,4);
    };
    const auto manager=saved[5];if (!manager) return 0;
    std::uint64_t a=0,b=0;
    if (Read(manager+0x60,a) && a) fix(a);
    if (Read(manager+0x68,b) && b) fix(b);
    fix(manager);return 0;
}
}
bool Install(std::uint8_t* image,std::size_t size) {
    base=image;image_size=size;
    if (!BbSettings::Get().widescreen) return false;
    for (const auto& s:stage) {
        std::uint8_t expected[9]={0x48,0x8d,0x05,0,0,0,0,0x8b,0};
        if(s.cx){expected[2]=0x0d;expected[8]=0x09;}
        const auto disp=std::int32_t(s.reads-(s.at+7));std::memcpy(expected+3,&disp,4);
        // The existing resolution patch pins nine reads already. Accept only its
        // exact native-stage instruction, never an arbitrary replacement.
        std::uint8_t pinned[9]={std::uint8_t(s.cx?0xb9:0xb8),0,0,0,0,0x0f,0x1f,0x40,0};
        std::memcpy(pinned+1,&s.value,4);
        if(!Matches(s.at,expected,9) && !Matches(s.at,pinned,9)) {
            std::printf("Engine: widescreen refused: Scaleform stage code differs at %zx\n",s.at);return false;
        }
    }
    constexpr float scale[]={0.5f,-0.5f};
    for(auto at:plates) if(!Matches(at,scale,8)) return false;
    constexpr float r=1920,b=1080;
    if(!Matches(0x4926ea4,&r,4)||!Matches(0x4926ea8,&b,4)) return false;
    for(const auto& site:left) if(!Matches(site.at,site.before.data(),site.n))return false;
    for(const auto& site:top) if(!Matches(site.at,site.before.data(),site.n))return false;
    constexpr std::uint8_t pro[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x48};
    Hook hook;if(!Prepare(hook,image,size,0x14368b0,pro,Blend))return false;
    for(const auto& s:stage){std::uint8_t code[9]={0xb8,0,0,0,0,0x0f,0x1f,0x40,0};if(s.cx)code[0]=0xb9;
        std::memcpy(code+1,&s.value,4);std::memcpy(base+s.at,code,9);}
    Commit(std::span<Hook>(&hook,1));installed=true;Tick();
    std::puts("Engine: bbhost widescreen enabled: native camera aspect and 15 Scaleform stage pins");return true;
}
void Tick() {
    if(!installed)return;
    const auto& settings=BbSettings::Get();
    const auto index=std::clamp(BbSettings::ResolutionNeedsRestart()?settings.startup_output_res:settings.output_res.load(),0,BbSettings::OutputCount-1);
    const float want=settings.widescreen?float(BbSettings::OutputWidths[index])/BbSettings::OutputHeights[index]:original_aspect;
    static float applied=0;if(want==applied)return;
    previous_aspect=aspect.load();aspect=want;
    const float rx=std::max(1.0f,want/original_aspect),ry=std::max(1.0f,original_aspect/want);
    const float scale[]={0.5f*rx,-0.5f*ry};
    const auto r=std::uint32_t(1920+std::ceil(960*(rx-1))),b=std::uint32_t(1080+std::ceil(540*(ry-1)));
    const float rf=float(r),bf=float(b);
    for(auto at:plates) WriteCode(base+at,scale,8);
    WriteCode(base+0x4926ea4,&rf,4);WriteCode(base+0x4926ea8,&bf,4);
    for(auto at:right_bounds)WriteCode(base+at+1,&r,4);
    for(auto at:bottom_bounds)WriteCode(base+at+1,&b,4);
    for(const auto& s:left)WriteCode(base+s.at,(rx>1.0005f?s.after:s.before).data(),s.n);
    for(const auto& s:top)WriteCode(base+s.at,(ry>1.0005f?s.after:s.before).data(),s.n);
    applied=want;std::printf("Engine: camera aspect %.4f, world plates x%u / y%u, vertical FOV preserved\n",want,r,b);
}
}
