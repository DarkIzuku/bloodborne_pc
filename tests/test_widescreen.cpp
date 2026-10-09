// SPDX-License-Identifier: GPL-3.0-or-later
// Synthetic image: no game executable or assets are needed.
#include "../gpu/shim/engine/widescreen.h"
#include "../gpu/shim/bbport_settings.h"
#include "../gpu/shim/bbport_platform.h"
#include <cassert>
#include <vector>
#include <cstring>
#include <cmath>
#include <cstdio>
#ifdef _WIN32
namespace BbPlatform {
bool ReadProcessMemory(const void* p,void* out,size_t n){if(!p)return false;std::memcpy(out,p,n);return true;}
}
#endif
int main(){
    std::vector<uint8_t> image(0x5130000);
    struct Stage {size_t at,reads;uint32_t v;bool cx=false;};
    const Stage stages[]={
        {0x15e83af,0x51289f8,1920},{0x1bfd491,0x51289f8,1920},{0x1bfd4d1,0x51289fc,1080},
        {0x1644357,0x51289f8,1920},{0x1644365,0x51289fc,1080},{0x1644c55,0x51289f8,1920},
        {0x1644c63,0x51289fc,1080},{0x16452c7,0x51289f8,1920},{0x16452d5,0x51289fc,1080},
        {0x1654df6,0x51289f8,1920},{0x1654e1c,0x51289fc,1080},{0x1655611,0x51289fc,1080},
        {0x1655628,0x51289f8,1920},{0x16b3372,0x51289f8,1920,true},{0x16b3398,0x51289fc,1080}};
    int index=0;for(auto s:stages) {
        uint8_t code[9]={0x48,0x8d,uint8_t(s.cx?0x0d:0x05),0,0,0,0,0x8b,uint8_t(s.cx?9:0)};
        int32_t disp=int32_t(s.reads-(s.at+7));std::memcpy(code+3,&disp,4);
        if(index++<9){code[0]=0xb8;std::memcpy(code+1,&s.v,4);const uint8_t nop[]={0x0f,0x1f,0x40,0};std::memcpy(code+5,nop,4);}
        std::memcpy(image.data()+s.at,code,9);
    }
    const float plate[]={.5f,-.5f},bounds[]={1920,1080};
    for(auto at:{0x48f9a00,0x48f9a60,0x48f9ad0})std::memcpy(image.data()+at,plate,8);
    std::memcpy(image.data()+0x4926ea4,bounds,8);
    struct Branch{size_t at;uint8_t bytes[3];size_t n;};
    const Branch branches[]={{0x1bfd473,{0x77,0x30},2},{0x1654dd9,{0x77,0x2f},2},
        {0x16b3370,{0x77,0x5a},2},{0x1642520,{0x0f,0x96,0xc1},3},
        {0x1bfd4cf,{0x77,0x14},2},{0x1654e1a,{0x77,0x14},2},{0x16b3396,{0x77,0x34},2},{0x164253c,{0x77,0x20},2}};
    for(auto b:branches)std::memcpy(image.data()+b.at,b.bytes,b.n);
    const uint8_t pro[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x48};
    std::memcpy(image.data()+0x14368b0,pro,sizeof pro);
    auto& s=BbSettings::Get();s.widescreen=true;s.output_res=s.startup_output_res=5;
    // A foreign modification must leave every hook/site untouched.
    image[stages[12].at]=0xcc;assert(!BbEngine::Widescreen::Install(image.data(),image.size()));
    assert(!std::memcmp(image.data()+0x14368b0,pro,sizeof pro));image[stages[12].at]=0x48;
    assert(BbEngine::Widescreen::Install(image.data(),image.size()));
    auto value=[&](size_t at){uint32_t v;std::memcpy(&v,image.data()+at,4);return v;};
    assert(value(0x1bfd491+1)==2250);assert(value(0x1bfd4d1+1)==1080);
    assert(image[0x1bfd473]==0x66 && image[0x1bfd4cf]==0x77);
    s.output_res=s.startup_output_res=8;BbEngine::Widescreen::Tick();
    assert(value(0x1bfd491+1)==2880);
    s.output_res=s.startup_output_res=1;BbEngine::Widescreen::Tick();
    assert(value(0x1bfd491+1)==1920 && image[0x1bfd473]==0x77);
    std::puts("Widescreen: PASS (existing native pins, foreign-byte rejection, 21:9/32:9 and 16:9 restoration)");
}
