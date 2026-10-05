# Windows launcher redesign

Based on `seamless-dev` at `d864807ed7359bc38c0c996388e0bd7c1c1c1b46`.
All changes are contained in `launcher-windows/BloodborneLauncher/`.

## Presentation

- Bundled hunter background, Bloodborne wordmark, title-bar image and application icon.
- Shared dark templates for buttons, navigation, dropdowns, checkboxes, text fields and scrollbars, with keyboard focus cues.
- Vector ornaments and window resize borders; scrollable pages and navigation on short screens.
- Five quick settings columns on wide windows, three on compact windows.
- Game-folder selection on Play, a Save button available on every page, and unsaved-change feedback.
- Bidirectional synchronization between quick settings and the existing Graphics/Performance controls.
- Existing game-folder/custom hero image discovery still takes precedence over the bundled background.

The supplied logo, hunter background and square icon are stored unchanged as PNG resources.
The ICO contains 16, 24, 32, 48, 64, 128 and 256 pixel versions of the supplied square image.
The reference mockup is not used as a flattened interface.

## Existing contracts

The configuration schema, INI keys, launch command and environment variables are unchanged.
All 14 original dropdown option lists and 17 checkboxes are retained. The original configuration,
detection, browse, mods and launch methods remain unchanged. The presentation partial class only
handles synchronization, window sizing, status text and responsive layout.

Existing behavior is retained where the reference is illustrative:

- A folder containing `eboot.bin` and `sce_sys` enables Play. Version detection reports 1.09, older
  versions or an unknown version without adding a new launch restriction.
- Vulkan and FSR 3.1 are fixed labels in the original launcher, not new capability probes.
- GPU detection still calls `out/bb-gpu-capabilities.exe`; runtime, DLSS and FSR 4 status still use
  the original file/folder checks.
- Display options remain borderless fullscreen and windowed; Quality remains the existing
  upscaler preset, not a newly introduced graphics preset.
- Developer switches remain gated by Developer Mode in the existing launch code.

No runtime, renderer, emulation, patches, networking, multiplayer or workflow changes are needed.

## Build and verify

On Windows with the .NET 8 SDK or a newer compatible SDK, from the repository root:

```powershell
dotnet build launcher-windows/BloodborneLauncher/BloodborneLauncher.csproj -c Release
dotnet publish launcher-windows/BloodborneLauncher/BloodborneLauncher.csproj -c Release -r win-x64 --self-contained true -p:PublishSingleFile=true -p:IncludeNativeLibrariesForSelfExtract=true -o launcher-publish
& ./launcher-windows/BloodborneLauncher/Verify-Launcher.ps1
```

`Verify-Launcher.ps1` creates a separate verification host under the Windows temporary directory.
It checks 116 assertions covering dump/version status, optional asset status, all quick settings
in both directions, the INI and JSON files, remembered game path, launch arguments/environment,
navigation, dropdown layout and short-screen scrolling. It renders the seven pages, compact
layouts and 144/192 DPI bitmap previews. An optional `-PreviewDirectory` selects their output
location; `-PackageCache` selects an existing NuGet package cache.

The verification host invokes the real launch handler against its own synthetic `run.bat`,
which records the received arguments and environment. It never invokes the real game runtime.
Test fixtures and previews are retained in the reported temporary directory for inspection.
The helper does not automate the native folder picker or Explorer; their existing handlers are
unchanged. GPU execution, actual upscaler support and game rendering still require a real runtime
and game dump on the reviewer's machine.

Validation for this revision: Release build (zero warnings/errors), self-contained Windows x64
publish, isolated executable startup, all 116 assertions, and visual review of the generated
pages/dropdown/compact previews.
