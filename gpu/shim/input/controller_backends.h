// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <SDL3/SDL.h>
// Both startup paths call this before joystick initialization. SDL deduplicates
// backends. Default-priority hints preserve user/environment overrides.
static inline void bb_controller_backends(void) {
#ifdef _WIN32
    if (!SDL_WasInit(SDL_INIT_JOYSTICK)) {
        SDL_SetHintWithPriority(SDL_HINT_JOYSTICK_WGI,"1",SDL_HINT_DEFAULT);
        SDL_SetHintWithPriority(SDL_HINT_JOYSTICK_GAMEINPUT,"1",SDL_HINT_DEFAULT);
    }
#endif
}
