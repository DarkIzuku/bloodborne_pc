# Bloodborne PC — DarkIzuku's Windows fork

A native Windows port of **Bloodborne**, with a WPF launcher, Vulkan rendering,
DLSS model selection, FSR upscaling, keyboard/mouse controls, ultrawide output
and focused stability improvements.

[Download the latest Windows release](https://github.com/DarkIzuku/bloodborne_pc/releases/latest)
· [Windows builds](https://github.com/DarkIzuku/bloodborne_pc/actions/workflows/windows-offline-build.yml)
· [Source branch](https://github.com/DarkIzuku/bloodborne_pc/tree/public-performance-stability-v2)

Based on [deadinside28's bbport](https://github.com/deadinside28/bloodborne_pc).
The game's x86-64 code runs directly on the CPU. The runtime replaces PS4 system
libraries, and a renderer derived from shadPS4 translates graphics to Vulkan.
This is an independent fork, not an official Sony, FromSoftware or shadPS4 release.

**No game files, firmware or saved games are included.** You need your own
compatible Bloodborne 1.09 dump. The port is experimental; automated tests do
not establish that every area, mod or hardware configuration works.

## Getting started

1. Download the Windows ZIP from **Releases** and extract the complete package
   into a writable folder. Keep its subfolders and DLLs together.
2. Open **BloodborneLauncher.exe** and select your Bloodborne game folder.
3. Choose display, graphics and control settings, save them and press **Play**.

The launcher validates the installation and automatically prepares the executable,
PC menus and selected mods. Only one game-folder selection is needed; there is
no separate executable picker. The dump must already have files in a supported
format: the launcher does not decrypt encrypted dumps or download game files.

The launcher hides while the game runs and returns when the game process exits.
Normal play does not require a persistent command window.

**Inicio rápido:** descarga y extrae el paquete completo, abre
`BloodborneLauncher.exe`, selecciona la carpeta de Bloodborne 1.09, guarda tus
opciones y pulsa **Play**. No hace falta compilar para utilizar una release.

## Requirements

- Windows 10 version 1803 or newer, or Windows 11; x86-64 CPU.
- Vulkan 1.3 capable GPU and compatible drivers; upscaler requirements vary.
- Your own supported Bloodborne 1.09 dump, including its required modules.
- Enough RAM, page-file space and storage for the game and local caches.

The portable release includes the launcher and its .NET runtime, Python for
local preparation, and redistributable dependencies. Development tools are only
needed when compiling from source.

## Features

- **WPF launcher:** persistent display, graphics, controls, camera, mods, online
  and diagnostic settings, with Bloodborne artwork and theme.
- **Native PC menus:** shared engine graphics/camera settings, FOV, keyboard
  bindings and local trophies, where supported by the validated 1.09 profile.
- **Display:** windowed/fullscreen output, 1080p/1440p/4K, 21:9 and 32:9
  ultrawide, centered UI and corrected camera projection.
- **Frame rate:** 30, 60, 90, 120 FPS and Unlimited. The title menu remains
  capped at 60 FPS. 120 FPS and Unlimited require FIFO VSync; Unlimited follows
  the display refresh limit with VSync. Hardware limits can reduce actual FPS.
- **DLSS Super Resolution / DLAA:** quality modes and independent NGX model
  hints (`Auto`, `A`–`F`, `J`, `K`, `L`, `M`). The installed NVIDIA DLL determines
  availability and can map older hints to newer models.
- **FSR 3.1, FSR 4, FSR 4.1.1 and native TAA:** temporal reconstruction, camera
  and object motion vectors, sharpening and native-resolution HUD composition.
  FSR 4 requires compatible Vulkan features/assets. FSR 4.1.1 assets must be
  generated locally from a compatible AMD DLL supplied by the user.
- **Controls:** remappable keyboard/mouse, native mouse camera, menu pointer
  and SDL gamepads. Recognized button textures show procedural keyboard/mouse
  or Xbox labels; PlayStation controllers retain the original glyphs. Mods
  replacing those textures keep their own prompts.
- **Gameplay options:** Hunter's Dream appearance editing, Altar of Despair
  respec, skip intro, free camera and debug options where supported. Debug menu
  fonts are not bundled and must be provided separately.
- **Mods and patches:** loose-file overlays, per-mod/per-patch switches, XML
  patches and bounded in-place executable-mod patches.
- **Diagnostics:** timestamped session logs, optional Detailed Logs/frame
  statistics, bounded crash dumps and an **Open Logs Folder** button.

Frame Generation is not enabled by these changes.

## Settings, saves and mods

`bbport.ini` stores runtime graphics/engine settings; `launcher-settings.json`
stores frontend preferences. Unrelated INI entries are preserved. Settings writes
use replacement files and retain the previous copy as `.bak`.

Saves normally live under `user/` in the selected data directory. Keep a separate
backup before changing runtimes or installing third-party mods. Updating a release
does not require deleting saves or existing configuration.

Put loose-file mods in separate folders under `mods/`. The loader supports
`dvdroot_ps4/`, common wrappers and bare game asset folders. Later mods in the
configured order override earlier files; originals are preserved.

Enabled mods may contain XML patches or a modified `eboot.bin`. Executable mods
are converted into bounded writes against a matching layout and never replace
the original executable. Invalid/truncated layouts and system-module replacement
are rejected. Third-party patches can still conflict with one another.

Online server addresses and accounts are user-configured; no server IP or account
is supplied by default. Trophies are stored locally, not uploaded as platform achievements.

## Stability and startup

This fork retains fixes for large Vulkan recording payloads, command lifetime,
texture overlap/writeback, loading-screen item icons, pipeline/shader caches,
Intel CPU compatibility and crash diagnostics.

The Windows shared guest memory pool reserves its address range and commits
pages when allocated, reducing the initial system commit charge. Released section
pages remain committed for reuse; this is not a VRAM decommit feature.

The executable cache verifies input/output hashes. Mod content, native menu
assets and selected patches are still processed at launch. Startup improvements
are not claims of higher gameplay FPS.

See [stability notes](docs/PUBLIC_STABILITY.md), [mods](docs/MODS.md) and
[frame pacing](docs/FRAME_PACING_WINDOWS.md).

## Building and testing

Windows development uses **MSYS2 CLANG64** (Clang, libc++, LLD, CMake/Ninja,
SDL3, Vulkan and the libraries listed in the workflow), plus **.NET 8** for WPF.
Clone with submodules; GitHub's source ZIP does not include them.

```sh
git clone --recursive --branch public-performance-stability-v2 https://github.com/DarkIzuku/bloodborne_pc.git
cd bloodborne_pc
bash build.sh
```

Run the build in CLANG64. Exact dependencies, test targets, launcher publishing
and portable packaging are defined by [the Windows workflow](.github/workflows/windows-offline-build.yml).
Prebuilt releases are recommended for players.

CI checks C++ regressions, frame pacing, preparation/patch safety, synthetic
crash capture and launcher settings/lifecycle. Gameplay, visual quality and
driver-specific behavior require additional runtime testing.

## Credits and licensing

Thanks to **deadinside28** for bbport, **shadPS4** and its contributors for the
renderer foundation, **droogie/bbhost** for engine/controls/menu work,
**AwesomeObserver**, **Awakenchan**, **Supermedo** and contributors for focused
stability research, and **Gerasimos Kanellatos (GerasimosKan)** for trophies,
menus, prompts, mods and Windows tooling.

Adaptation details and third-party notices are retained in [licenses/](licenses).
Existing code retains its original notices, including GPL-2.0-or-later code;
adaptations using its later-version grant are identified as GPL-3.0-or-later.
See [LICENSE](LICENSE) and individual source headers. NVIDIA/AMD components
and other dependencies retain their own terms.
