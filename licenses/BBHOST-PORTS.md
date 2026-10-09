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

These portions are GPL-3.0-or-later. Copyright remains with the bbhost contributors.
The surrounding bloodborne_pc code remains GPL-2.0-or-later under its existing notices.
Combined builds containing these ports are distributed under GPL-3.0-or-later; see
`GPL-3.0.txt`. Third-party notices remain unchanged. Generated game assets are local
cache entries from the player's own dump and are not distributed in the repository.
