// SPDX-License-Identifier: GPL-3.0-or-later
// Narrow bridge to bloodborne_pc's existing loader and engine ABI, not bbhost's shell.
#pragma once
#include "../engine/engine_hooks.h"
#include "../bbport_platform.h"
#include <cstdint>
#include <cstddef>
#include <string>
#define GUEST_ABI __attribute__((sysv_abi))
constexpr uint64_t kPreferredGuestSlide=0x400000;
constexpr const char* kEboot109Sha256="071df19c8880086d97182dbc057bc8cb37badaca57d9112683836b24a0444c0a";
struct GuestMemory { uint64_t slide;size_t size; };
struct ElfImage { GuestMemory mem;std::string sha256; };
inline void* guest_ptr(const GuestMemory& m,uint64_t va) {
    return va>=m.slide && va-m.slide<m.size?reinterpret_cast<void*>(va):nullptr;
}
// These adapters are used ONLY during Install, while this loader owns the RW image.
// The loader applies segment protections afterward. Runtime changes use WriteCode.
inline bool guest_protect_rw(GuestMemory* m,uint64_t va,size_t n) {
    return va>=m->slide && va-m->slide<=m->size && n<=m->size-(va-m->slide);
}
inline bool guest_protect_rwx(GuestMemory* m,uint64_t va,size_t n) {return guest_protect_rw(m,va,n);}
inline bool guest_protect_rx(GuestMemory*,uint64_t,size_t) {return true;}
inline bool host_read_safe(const void* p,void* out,size_t n) {return BbPlatform::ReadProcessMemory(p,out,n);}
inline void* hle_wrap_fn(void* p) {return p;}
template<typename T=std::int64_t,typename... A> T hle_call_guest(void* p,A... a) {
    return static_cast<T>(BbEngine::Call(reinterpret_cast<uint64_t>(p),a...));
}
bool input_prologue_hook(ElfImage*,uint64_t,const uint8_t*,size_t,BbEngine::Callback);
void host_log(const char*,...);
constexpr uint8_t kStickRest=127,kStickLow=0,kStickHigh=254;
constexpr int kPadTouchW=1920,kPadTouchH=943;
struct PadState {
    uint32_t buttons=0;
    uint8_t lx=127,ly=127,rx=127,ry=127,l2=0,r2=0;
    bool connected=true;
    uint64_t timestamp=0;
    struct Touch {uint16_t x=0,y=0;uint8_t id=0;bool down=false;} touch[2];
    uint8_t touch_count=0;
    uint32_t menu_buttons=0;
};
enum class InputDevice { KeyboardMouse,Pad };
struct HostSettings {bool mouse_menu;};
HostSettings host_settings();
InputDevice host_input_device();
PadState host_pad_state();
bool host_mouse_stage_position(float&,float&);
bool host_mouse_relative();
void host_mouse_describe(char*,size_t);
bool host_options_open();
bool ingame_menu_open();
bool host_text_entry_open();
bool debug_menu_open();
bool debug_menu_active();
void menu_pointer_install(ElfImage*);
void menu_pointer_click();
uint32_t menu_pointer_take_press();
uint64_t menu_pointer_tick();
bool menu_pointer_in_menu();
uint32_t menu_region_confirm_button();
uint32_t menu_confirm_button();
uint32_t menu_back_button();
bool option_menu_click_decides(uint8_t*,int);
void option_menu_list_update(uint8_t*,bool);
void mouse_camera_install(ElfImage*);
bool mouse_camera_installed();
float mouse_camera_degrees_per_count(int);
void mouse_camera_publish(int,bool,bool,bool);
bool mouse_camera_flick(float&,float&);

