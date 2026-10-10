// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif
#ifdef __cplusplus
union SDL_Event;
struct SDL_Window;
namespace BbInput {
bool Install(uint8_t* image,size_t size);
bool Enabled();
void Event(const SDL_Event&,bool blocked);
void Pump(SDL_Window*,bool text_active);
void Tick();
}
extern "C" {
#endif
typedef struct {
    uint32_t buttons;
    uint8_t lx,ly,rx,ry,l2,r2,touch_count;
    struct { uint16_t x,y;uint8_t id; } touch[2];
} BbPcPad;
int bbgpu_pc_input(BbPcPad*,const bool* keys,int nkeys,int gamepad_present);
#ifdef __cplusplus
}
#endif
