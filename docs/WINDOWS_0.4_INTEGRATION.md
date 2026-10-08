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

Compiling successfully does not establish visual correctness. The item-card loading screen
must be observed over repeated loads with temporal upscaling both enabled and disabled. The
0.4 notes identify a possible relation between the constant RAM race and flickering glyphs,
but explicitly say that relation is unverified. Do not describe it as fixed before testing.

## Requested bbhost features

Ultrawide, native game-style PC menus, the Dream mirror appearance editor, altar rebirth and
mouse/keyboard handling are separate ports from bbhost's engine implementations. They are not
features of upstream bloodborne_pc 0.4. Inspect their hooks, 1.09 byte checks, script/asset
generation and host dependencies before adapting them; preserve this renderer and upscalers.
