# Windows launcher: default game folder and optimizations

Date: 2026-10-10
Scope: `launcher-windows/BloodborneLauncher/` only (the WPF `BloodborneLauncher.exe`).
The Python/GTK launcher in `launcher/`, the runtime, and the configuration schema are untouched.

## Goal

The launcher should find the game on first run in a portable install and be smaller,
safer with the user's settings, and consistently in English. The look and the pages stay
as they are.

## Portable layout (example)

```
D:\Bloodborne\                 <- launcher root (folder with run.bat + build.sh)
  BloodborneLauncher.exe
  CUSA03173\                   <- game dump (eboot.bin, sce_sys)
  bbport.ini, launcher-settings.json, user\, mods\, patches\, logs\, out\ ...
```

## Changes

1. **Default game folder.** If no game path is saved, use `<root>\CUSA03173`.
   If the saved path has no `eboot.bin` but `<root>\CUSA03173` does, use the default.
   A valid folder the user picked always wins.
2. **Smaller exe.** `EnableCompressionInSingleFile` in the project file, so every
   single-file publish (local and CI) is compressed. Still self-contained: no .NET install needed.
3. **GPU detected once.** `bb-gpu-capabilities.exe` runs once per launcher session instead
   of on every Save/Browse.
4. **Safe saves.** `launcher-settings.json` and `bbport.ini` are written to a temp file and
   swapped in, keeping the previous version as `.bak`. A crash mid-save can no longer leave
   an empty or truncated file.
5. **Damaged settings.** If `launcher-settings.json` cannot be read, load `.bak` and say so in
   the footer, instead of silently resetting everything.
6. **INI read once.** `bbport.ini` is parsed once per load instead of once per key (44 reads).
7. **Background as JPEG.** `hunter-background.png` (2.4 MB, no alpha) becomes a JPEG.
8. **English UI.** Spanish strings on the Controller and Online pages, key binding actions and
   error messages are translated to English.

## Not changing

- Visual design, page structure, settings schema, INI keys, launch command and environment.
- Framework-dependent publish (smallest exe) was considered and rejected: it would require users
  to install the .NET 8 Desktop Runtime.

## Build and package

- .NET 8 SDK, `dotnet publish ... -p:PublishSingleFile=true` (see REDESIGN.md), then
  `Verify-Launcher.ps1`.
- Portable zip following the CI layout (`.github/workflows/windows-offline-build.yml`):
  launcher, `run.bat`, `build.sh`, `setup.bat`, runtime executables and DLLs from `out\`,
  `scripts`, `patches`, `fsr4_shaders`, `tools`, `licenses`, empty `mods`.
  Never included: the game dump, saves (`user`), logs, personal `bbport.ini` /
  `launcher-settings.json`, and game-derived files in `out\` (`eboot.elf`, `*.bin`).
