// SPDX-License-Identifier: GPL-3.0-or-later
#include "../gpu/shim/input/bindings.h"
#include "../gpu/shim/input/compat.h"
#include "../gpu/shim/input/mouse_camera_step.h"
#include <SDL3/SDL.h>
#include <cassert>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
extern "C" {std::uint8_t bb_mouse_camera_free_now=0;}
void host_log(const char*,...) {}
bool debug_menu_active(){return false;}
uint32_t menu_confirm_button(){return 0x4000;}
uint32_t menu_back_button(){return 0x2000;}
int main(){
    host_bindings_load_defaults();
    assert(host_binding_key(kBindWalk)==SDL_SCANCODE_LALT);
    assert(host_binding_mouse(kBindAttack)==1);
    assert(host_binding_ini_value(kBindAttack)=="Mouse1");
    bool h[kBindCount]={};h[kBindMoveF]=true;
    PadState p;host_bindings_apply(h,false,p);assert(p.lx==127 && p.ly==0);
    h[kBindMoveR]=true;p=PadState{};host_bindings_apply(h,false,p);assert(p.lx==254 && p.ly==0);
    h[kBindWalk]=true;p=PadState{};host_bindings_apply(h,false,p);
    const float diagonal=std::hypot(float(p.lx)-127,float(p.ly)-127);
    h[kBindMoveR]=false;p=PadState{};host_bindings_apply(h,false,p);
    assert(std::abs(diagonal-(127-p.ly))<=1);
    std::memset(h,0,sizeof h);h[kBindAttack]=true;p=PadState{};host_bindings_apply(h,true,p);
    assert(p.buttons==0x200 && p.r2==255);
    assert(host_bindings_parse("attack","E, Mouse2"));
    assert(host_binding_ini_value(kBindAttack)=="E, Mouse2");
    host_bind_capture_begin(kBindAttack);assert(host_bind_capture_key(SDL_SCANCODE_F));
    assert(host_binding_key(kBindAttack)==SDL_SCANCODE_F && host_binding_key(kBindUseItem)==0);
    assert(host_bind_capture_blocking());host_bind_capture_note_held(false);assert(!host_bind_capture_blocking());
    host_bind_capture_begin(kBindAttack);host_bind_capture_key(SDL_SCANCODE_ESCAPE);
    assert(host_binding_key(kBindAttack)==SDL_SCANCODE_F);host_bind_capture_note_held(false);
    host_binding_clear(kBindAttack);assert(host_binding_ini_value(kBindAttack)=="none");
    float sensitivity=mouse_camera::degrees_per_count(5);assert(std::abs(sensitivity-.25f)<1e-6);
    uint32_t bits;std::memcpy(&bits,&sensitivity,4);mouse_camera::g_degrees_per_count=bits;
    auto turn=[](int frames){
        float pitch=0,yaw=0;
        for(int i=0;i<frames;i++){
            mouse_camera::add(mouse_camera::g_counts,120.0f/frames,60.0f/frames);
            mouse_camera::begin_update();mouse_camera::turn(0,0,pitch,yaw,-1,1);
        }
        return std::pair{pitch,yaw};
    };
    const auto a=turn(30),b=turn(60),c=turn(90);
    assert(std::abs(a.first-b.first)<1e-5 && std::abs(a.second-c.second)<1e-5);
    float pitch=0,yaw=0;mouse_camera::g_frame={10,10};
    assert(!mouse_camera::turn(1,0,pitch,yaw,-1,1) && pitch==0 && yaw==0);
    mouse_camera::g_flags=mouse_camera::kInvertX|mouse_camera::kInvertY;
    assert(mouse_camera::turn(0,0,pitch,yaw,-1,1) && pitch<0 && yaw<0);
    mouse_camera::g_frame={10000,10000};mouse_camera::turn(0,0,pitch,yaw,-.5,.5);
    assert(pitch==-.5f && yaw>=-3.141593f && yaw<=3.141593f);
    std::puts("PC input: PASS (walk/sprint axes, remapping, capture, triggers, 30/60/90 camera, invert/limits)");
}
