# bbhost engine ports

Source: https://github.com/droogie/bbhost/tree/7c790536c2c27ad7bb5115e3b1a12d7ffd7a972e

`gpu/shim/engine/engine_hooks.cpp` adapts `src/core/thunk.cpp` (the prologue emitter).
`gpu/shim/engine/change_appearance.cpp` adapts `src/engine/change_appearance.cpp`,
`menu_steps.cpp`, `frame_rate.cpp` (only the main-thread hook site) and `world_chr.cpp`
(only the player block lookup). `scripts/engine_assets.py` adapts
`dream_mirror_layout.cpp` and `gcn/container.cpp` (DCX).
`gpu/shim/engine/camera.cpp` adapts `engine/camera.cpp` and the typed repository
lookup in `engine/params.cpp`. Its sliders and the WPF launcher share the existing
`bbport.ini`; no bbhost renderer or second configuration store is introduced.
`gpu/shim/engine/option_menu.cpp` adapts the native row, caption, section opener and
System builder from `engine/option_menu.cpp`, and its title menu Quit Game (the
finalise call-site row and the native YES/NO opener, from bbhost c421ea0, with
`menu_assets.cpp`'s title list slot). `menu_memory.cpp` adapts bbhost's
Scaleform heap sizes. `tools/engine/menu_assets.cpp` adapts the independent GFX,
BND4 and FMG serializers from `engine/menu_assets.cpp`, with bounded inflation;
its PC strings come from `tools/engine/pc_option_messages.tsv`. All edited game movies
and language bundles are prepared locally, never packaged or committed.
`engine_state.cpp` adapts typed lookups from `params.cpp`, `player_data.cpp`,
`event_flags.cpp`, `world_chr.cpp` and the shared `menu_steps.cpp` watches.
`event_flag_layout.h` retains the traversal from `decomp/sprj_event_flag_man.h`.
`rebirth.cpp` retains the native apply/heal, refund, snapshot/undo and menu
lifecycle from `engine/rebirth.cpp`. The origin pricing in `rebirth_refund.h`
adds rejection of invalid attributes, non-finite curves and overflowing refunds.
`tools/engine/esd.*`, `rebirth_script.*` and `sha256.*` adapt the corresponding
bbhost serializers and digest helper. Both known altar script identities are
checked before and after preparation.
`graphics.cpp` adapts the render-view flags and freshly blended CPU parameters
from `engine/graphics_patch.cpp`. Fog, renderer replacement and shader patches
are not included; motion blur, DoF and CA keep bloodborne_pc's startup patches.

These portions are GPL-3.0-or-later. Copyright remains with the bbhost contributors.
The surrounding bloodborne_pc code remains GPL-2.0-or-later under its existing notices.
Combined builds containing these ports are distributed under GPL-3.0-or-later; see
`GPL-3.0.txt`. Third-party notices remain unchanged. Generated game assets are local
cache entries from the player's own dump and are not distributed in the repository.

Keyboard/mouse and widescreen source:
https://github.com/droogie/bbhost/tree/db5457cbd447f4ef129b58df0a78e0a8819c7033

The input/bindings, mouse_camera and menu_pointer modules adapt the matching
bbhost host and engine modules. They retain angle-per-count camera movement,
controller priority, target flick, walking, region-aware confirmation, Scaleform
geometry hit tests and capture semantics. input.cpp bridges the existing SDL
event pump, scePad ABI, checked loader image and guest-call bridge.
Native PC Controls / Key Bindings reuse the row builders, checked widget identity
and Defaults lifecycle from option_menu.cpp. Both frontends use bbport.ini.

engine/widescreen.cpp adapts the camera blend hook, UI stage pins and projected
world-plate bounds from live_resolution.cpp and graphics_patch.cpp. It checks
original bytes or the exact existing native-stage resolution patch, preserves
vertical FOV and uses this port's output size. Vulkan UI composition adds uniform
centered fitting, including stencil masks and window-space vertices. The existing
presenter already letterboxes. The existing renderer, temporal resources,
synchronization, shader and motion-vector paths remain in use.
