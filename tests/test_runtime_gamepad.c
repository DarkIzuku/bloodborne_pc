// SPDX-License-Identifier: GPL-3.0-or-later
// Runs the actual runtime discovery code with SDL virtual gamepads. This is not
// evidence that an unconnected physical ASUS controller has been tested.
#include <assert.h>
#include "../src/runtime_pad.c"
// Unused native dispatch/overlay services. Any accidental exercise of the
// native export table fails this discovery-only test instead of faking a game.
uintptr_t runtime_lookup(const RuntimeExport *table,size_t count,const char *nid) {
    (void)table; (void)count; (void)nid; assert(!"native runtime dispatch in controller test"); return 0;
}
uint64_t host_monotonic_ns(void) { return SDL_GetTicksNS(); }
int bbgpu_overlay_captures_input(void) { return 0; }
int bbgpu_pc_input(BbPcPad *pad,const bool *keys,int count,int present) {
    (void)pad; (void)keys; (void)count; (void)present; return 0;
}
int main(void) {
    _putenv_s("BB_GAMEPAD","Bloodborne test pad");
    SDL_ResetHint(SDL_HINT_JOYSTICK_GAMEINPUT); SDL_ResetHint(SDL_HINT_JOYSTICK_WGI);
    bb_controller_backends();
    assert(SDL_GetHintBoolean(SDL_HINT_JOYSTICK_GAMEINPUT,false));
    assert(SDL_GetHintBoolean(SDL_HINT_JOYSTICK_WGI,false));
    SDL_SetHintWithPriority(SDL_HINT_JOYSTICK_GAMEINPUT,"0",SDL_HINT_OVERRIDE);
    SDL_SetHintWithPriority(SDL_HINT_JOYSTICK_WGI,"0",SDL_HINT_OVERRIDE);
    bb_controller_backends();
    assert(!SDL_GetHintBoolean(SDL_HINT_JOYSTICK_GAMEINPUT,true));
    assert(!SDL_GetHintBoolean(SDL_HINT_JOYSTICK_WGI,true));
    assert(SDL_Init(SDL_INIT_GAMEPAD));
    SDL_VirtualJoystickDesc descriptor={0};
    descriptor.version=sizeof(descriptor);
    descriptor.type=SDL_JOYSTICK_TYPE_GAMEPAD;
    descriptor.naxes=SDL_GAMEPAD_AXIS_COUNT;
    descriptor.nbuttons=SDL_GAMEPAD_BUTTON_COUNT;
    descriptor.name="Bloodborne test pad";
    SDL_JoystickID id=SDL_AttachVirtualJoystick(&descriptor);
    assert(id && SDL_IsGamepad(id));
    SDL_UpdateJoysticks();
    SDL_Gamepad *selected=current_gamepad();
    assert(selected && SDL_GetGamepadID(selected)==id);
    SDL_Joystick *joystick=SDL_GetGamepadJoystick(selected);
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_SOUTH,true));
    SDL_UpdateJoysticks();
    assert(SDL_GetGamepadButton(selected,SDL_GAMEPAD_BUTTON_SOUTH));
    assert(SDL_DetachVirtualJoystick(id));
    SDL_UpdateJoysticks();
    SDL_Delay(1050);
    selected=current_gamepad();
    assert(!selected || SDL_GetGamepadID(selected)!=id);
    id=SDL_AttachVirtualJoystick(&descriptor);
    assert(id);
    SDL_UpdateJoysticks(); SDL_Delay(1050);
    selected=current_gamepad();
    assert(selected && SDL_GetGamepadID(selected)==id);
    SDL_CloseGamepad(gamepad); gamepad=NULL;
    assert(SDL_DetachVirtualJoystick(id));
    SDL_Quit();
    puts("Runtime gamepad: selection, buttons, disconnect/reconnect and backend overrides passed");
    return 0;
}
