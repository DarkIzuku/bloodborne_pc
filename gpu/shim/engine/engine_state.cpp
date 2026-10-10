// SPDX-License-Identifier: GPL-3.0-or-later
// Typed source layouts from bbhost at 7c790536; no RAM scanning or save-file parsing.
#include "engine_state.h"
#include "event_flag_layout.h"
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
namespace BbEngine {
namespace {
std::uint64_t slide;
template<typename T> bool Read(std::uint64_t at,T* out) { return at && host_read_safe(reinterpret_cast<void*>(at),out,sizeof(T)); }
std::uint64_t Pointer(std::uint64_t at) { std::uint64_t p=0;Read(at,&p);return p; }
std::uint64_t Slot(std::uint64_t bn) { return slide+(bn-kPreferredGuestSlide); }
struct Stat { const char* name; std::uint32_t offset; };
constexpr Stat stats[]={{"hp",0x14},{"vitality",0x40},{"endurance",0x48},{"strength",0x58},{"skill",0x60},{"bloodtinge",0x68},{"arcane",0x70},{"level",0x90},{"echoes",0x94}};
std::uint64_t Record() { auto m=Pointer(Slot(0x593b130)); return m?Pointer(m+8):0; }
struct Watch { std::uint64_t type=0;const char16_t* name=nullptr;std::atomic<std::uint64_t> made{0}; };
Watch watches[8]; int count;
struct Safe { template<typename T> bool operator()(std::uint64_t at,T* out) const { return Read(at,out); } };
}
bool host_read_safe(const void* p,void* out,std::size_t n) { return p && BbPlatform::ReadProcessMemory(p,out,n); }
void host_log(const char* format,...) { std::fputs("Engine: ",stdout);va_list a;va_start(a,format);std::vprintf(format,a);va_end(a);std::putchar('\n'); }
void StateInitialize(std::uint64_t image) { slide=image; }
bool player_stat_get(const char* name,std::int64_t* value) {
    auto record=Record(); if (!record) return false;
    for (const auto& s:stats) if (!std::strcmp(name,s.name)) { std::int32_t v=0;if (!Read(record+s.offset,&v)) return false;*value=v;return true; }
    return false;
}
bool player_stat_set(const char* name,std::int64_t value) {
    auto record=Record(); if (!record) return false;
    for (const auto& s:stats) if (!std::strcmp(name,s.name)) { std::int32_t current=0;if (!Read(record+s.offset,&current)) return false;const auto v=static_cast<std::int32_t>(value);std::memcpy(reinterpret_cast<void*>(record+s.offset),&v,4);return true; }
    return false;
}
bool player_origin(int* origin) { std::uint8_t v=0;auto r=Record();if (!r || !Read(r+0xce,&v) || v>8) return false;*origin=v;return true; }
bool world_player_block(std::uint32_t* block) {
    auto m=Pointer(Slot(0x593e878)),chr=m?Pointer(m+0x60):0;
    std::uint32_t v=0xffffffff;if (!chr || !Read(chr+0x3f8,&v) || v==0xffffffff) return false;
    if (block) *block=v;return true;
}
bool event_flag_get(std::uint32_t id,bool* value) {
    const auto man=Pointer(Slot(0x593b100));std::uint64_t at=0;std::uint8_t mask=0,v=0;
    if (!man || !sprj_event_flag::locate(man,id,Safe{},&at,&mask) || !Read(at,&v)) return false;
    if (value) *value=(v&mask)!=0;return true;
}
bool event_flag_set(std::uint32_t id,bool value) {
    const auto man=Pointer(Slot(0x593b100));std::uint64_t at=0;std::uint8_t mask=0,v=0;
    if (!man || !sprj_event_flag::locate(man,id,Safe{},&at,&mask) || !Read(at,&v)) return false;
    v=value?static_cast<std::uint8_t>(v|mask):static_cast<std::uint8_t>(v&~mask);
    std::memcpy(reinterpret_cast<void*>(at),&v,1);return true;
}
void* params_row(const char* wanted,std::uint32_t id,std::size_t* size) {
    const auto repo=Pointer(Slot(0x5940340)); if (!repo || !wanted) return nullptr;
    for (unsigned s=0;s<63;++s) {
        const auto entry=repo+0x70+s*0x48;std::int32_t n=0;if (!Read(entry,&n) || n<=0) continue;
        for (unsigned k=0;k<static_cast<unsigned>(n) && k<8;++k) {
            const auto cap=Pointer(entry+8+8*k);std::uint64_t capacity=0;if (!cap || !Read(cap+0x30,&capacity)) continue;
            const auto name=capacity<8?cap+0x18:Pointer(cap+0x18);
            bool same=true; unsigned c=0;
            for (;c<64;++c) { char16_t ch=0;if (!Read(name+2*c,&ch) || ch!=static_cast<unsigned char>(wanted[c])) { same=false;break; }if (!ch) break; }
            if (!same || c==64) continue;
            auto holder=Pointer(cap+0x70),file=holder?Pointer(holder+0x70):0;
            std::uint16_t rows=0;std::uint8_t format=0,flags=0;
            if (!file || !Read(file+0xa,&rows) || !rows || !Read(file+0x2d,&format) || !Read(file+0x2e,&flags)) continue;
            auto record=[&](unsigned index,std::uint32_t* rid,std::uint64_t* data) {
                auto at=file+(format==2?0x34:0x40)+(format>3 && (flags&2)?0x18:0xc)*index;
                std::uint64_t offset=0;
                if (!Read(at,rid)) return false;
                if (format>3 && (flags&2)) { if (!Read(at+8,&offset)) return false; }
                else { std::uint32_t off=0;if (!Read(at+4,&off)) return false;offset=off; }
                if (offset<0x34 || offset>64*1024*1024) return false;*data=file+offset;return true;
            };
            unsigned lo=0,hi=rows;
            while (lo<hi) {
                unsigned mid=lo+(hi-lo)/2;std::uint32_t rid=0;std::uint64_t data=0;
                if (!record(mid,&rid,&data)) return nullptr;
                if (rid==id) {
                    std::uint64_t a=0,b=0;std::uint32_t unused=0;
                    if (size) *size=rows>1 && record(0,&unused,&a) && record(1,&unused,&b) && b>a && b-a<65536?b-a:0;
                    return reinterpret_cast<void*>(data);
                }
                if (rid<id) lo=mid+1;else hi=mid;
            }
        }
    }
    return nullptr;
}
int menu_steps_watch(std::uint64_t type,const char16_t* name) {
    if (count>=8 || !name) return -1;const int w=count++;watches[w].type=type;watches[w].name=name;return w;
}
void menu_steps_notify(const std::uint64_t* saved) {
    if (!saved[2]) return;
    for (int w=0;w<count;++w) {
        auto& x=watches[w]; if (saved[3]!=x.type) continue;
        unsigned i=0;
        for (;i<64;++i) { char16_t c=0;if (!Read(saved[2]+2*i,&c) || c!=x.name[i]) break;if (!c) { x.made.store(saved[5],std::memory_order_release);break; } }
    }
}
std::uint64_t menu_steps_take(int w) { return w>=0 && w<count?watches[w].made.exchange(0,std::memory_order_acq_rel):0; }
void menu_step_release(std::uint64_t step) { if (--*menu_step_count(step)==0) Call(Pointer(Pointer(step)),step); }
}
