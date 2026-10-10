# Windows integration of upstream 0.4

Integration branch: `upstream-0.4-core-port`.

- Custom base: `f62f2da39e7f92058465abd112df26e2c6256e31` (the installed Windows build).
- Upstream: `deadinside28/bloodborne_pc`, tag `0.4`,
  `daa71654083b42ea0730b3f0fecee2fc20942d02`.
- Existing stable branches, the installed game dump and its saves are not modified.

## Preserved

The WPF launcher and artwork, Win32 section-backed guest memory, native Windows threads and
clocks, vectored exception reports, DLSS/NGX (runtime 310.9.1), the FSR backends, atomic Windows
shader-cache writes, cache preload progress, mod directory junctions and selected controller
index remain. A controller GUID from 0.4 takes precedence when supplied.

## Upstream corrections

The merge includes viewport-derived vertical camera motion, vertex/index descriptor snapshots
and pending-read tracking before constant RAM writes, streamed texture upload ordering,
overlap-only host copy waits, ordered parallel Vulkan recording, minimized surface handling,
CPU instruction checks, corrupted cache recovery, touch IDs/hold time, simultaneous keyboard
and gamepad input, and settings saves which preserve launcher bindings.

DLSS keeps its own format metadata and sharpening. FSR's new encoded-to-linear conversion and
reactive-mask output blend are confined to the FSR path.

The optional Linux dma-buf memory model and SIGTRAP-based heap/libGnm experiments are not
enabled on Windows: they need a separate implementation for Windows sections and exception
contexts. Windows keeps its established memory model and existing crash reports. Linux keeps
the upstream implementation. GPU wait stack tracing is adapted to Windows APIs.

## Validation

CI builds the Windows runtime with MSYS2 CLANG64, publishes the .NET 8 WPF launcher and bundles
its existing runtimes/assets. It runs cache, motion, image-overlap and settings regressions,
plus preparation, patch, version-detection and upscaler-asset Python tests.

The loading-card correction preserves Scaleform stencil-only mask draws when UI composition
is redirected to native output resolution. Previously those zero-color-attachment draws did
not enter the UI phase; moving the subsequent icon draw cleared the guest mask. Captured
draw traces and repeated DLSS Quality 2560x1440 loads reproduced the missing icon before
the fix and visible item images after it. The user independently confirmed the correction.
The UI composition regression covers the observed clip shaders and rejects scene shadows.

Windows keeps the 0.3 cursor behavior and removes Unlimited FPS. The previous diagnostic
runs used uncapped presentation, where the user observed approximately 2000 FPS in the menu
and desktop freezes. The 60 FPS test completed without that problem. The launcher migrates
retired selections to 60; both run_windows.py and direct runtime launches enforce supported
30/60/90 caps, overriding legacy unbounded settings. Linux's rate options are unchanged.
Do not attribute the freeze to recording threads or queue changes: those diagnostic changes
were inconclusive and are not included. Original saves matched their hashes after testing.
Full preset/resolution and long-play validation remains pending.

## Requested bbhost features

Ultrawide, native game-style PC menus, the Dream mirror appearance editor, altar rebirth and
mouse/keyboard handling are separate ports from bbhost's engine implementations. They are not
features of upstream bloodborne_pc 0.4. Inspect their hooks, 1.09 byte checks, script/asset
generation and host dependencies before adapting them; preserve this renderer and upscalers.

These engine ports now live on bbhost-enhancements-v1. Keyboard/mouse and widescreen use
bbhost db5457cbd447f4ef129b58df0a78e0a8819c7033: raw camera counts, controller priority,
target switching flick, Alt walking, region-aware confirmation, actual Scaleform list hit
testing and native key capture. Output choices include six ultrawide modes. Native UI,
including loading-card stencil masks, fits the centered 1920x1080 stage uniformly; scene
render size and DLSS continue through the existing renderer.

The widescreen installer accepts both original stage reads and the exact native stage
pins supplied by this port's resolution patch. Synthetic-image regressions reject foreign
code and verify 21:9/32:9 bounds and restoration to 16:9. Native input regressions cover
neutral/sprint axes, diagonal walking, rebinding, capture release, triggers and identical
mouse turns at 30/60/90 updates. The WPF verification adds INI persistence and duplicate
Play protection. A joint DLSS Quality / 3440x1440 / 60 FPS gameplay test completed normally
and the user confirmed it looked and controlled correctly. Original saves remain untouched.

Still pending: two-client online gameplay and an in-game altar test once the save reaches it.
Real 32:9 monitor testing and long sessions across multiple areas remain broader validation.
