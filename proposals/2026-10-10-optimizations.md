# Optimization proposal — 2026-10-10

Scope: what the inspection found, measured where it could be measured, ordered by
payoff per line changed. Items marked "no action" were inspected and look fine;
they are listed so nobody re-audits them.

## Measured: the per-launch preparation pipeline (~12 s every start)

`scripts/run_windows.py` (and `run.sh`) runs the whole preparation chain on **every**
launch, including the in-game "Apply and restart" path. Timed on this machine, warm
disk cache, game at `C:\Users\geras\Desktop\Bloodborne\CUSA03173`:

| Step | Time | Notes |
|---|---|---|
| `python -c pass` | 0.11 s | interpreter spawn, paid 7× |
| `mods.py` | 0.32 s | 0 mods |
| `prepare.py` | **8.74 s** | of which `rglob('dvdroot_ps4')` + `stat` of 28 850 files = **5.75 s** |
| `link_libc.py` | 1.14 s | re-reads + re-hashes eboot and libc |
| `link_modules.py` | 1.08 s | re-reads + re-hashes eboot and modules |
| `content_profile.py` | 0.22 s | |
| `patches.py --print-scaled` | 0.23 s | |
| `patches.py` (full) | 0.30 s | |
| `engine_assets` (cached path) | 0.53 s | already content-addressed, fine |
| **Total** | **~12.7 s** | all outputs are deterministic in their inputs |

Inside `prepare.py` the real work is small: read eboot 0.03 s, parse SELF 0.06 s,
image SHA-256 0.19 s. The rest is the directory walk and the import/relocation pass.

### 1. Drop the `dvdroot_ps4` walk in `prepare.py` — saves 5.75 s, ~6 lines

`scripts/prepare.py:233-247` walks and stats every file under `dvdroot_ps4` only to
put per-folder counts and `resource_bytes` into `out/analysis.json`. Nothing in
`scripts/`, `launcher/`, `src/` or `tests/` reads those keys (only a sentence in
`docs/DEVELOPMENT_LOG.ru.md` mentions them).

- Change: delete the walk and the two report keys; update the one doc sentence.
- Tests: `tests/test_prepare.py` does not assert on `resources`; run it unchanged.
- Risk: none functional. If the counts are wanted for bug reports, gate the walk
  behind `--report` instead of deleting (same saving on the launch path).

### 2. Skip `prepare`/`link_libc`/`link_modules`/`content_profile` when inputs are unchanged — saves the remaining ~6 s, ~25 lines

All four scripts are pure functions of: `eboot.bin`, `sce_module/*`, `sce_sys/param.sfo`,
the merged mods path, `BB_CONTENT_SKU`, and the scripts themselves. Write a stamp
`out/prepare-stamp.json` after a successful chain with, for each input, `(size, mtime_ns)`
plus the SHA-256 of each script file; on the next launch compare and skip the chain when
equal. `BB_FORCE_PREPARE=1` bypasses it.

- Where: one helper in `scripts/run_windows.py` around the loop at line 142; mirror it
  in `run.sh` lines 64-66 so Linux gets the same behaviour (or call the same Python helper).
- Tests: one test in `tests/test_run_settings.py` style: stamp matches → skipped; touch
  eboot mtime or edit a script → re-run.
- Risk: a stale output if an input is missing from the key. The key above covers every
  path the four scripts open (`read_bytes`/`iterdir` call sites were checked). The game
  check in `game_check.py` is part of `prepare.py`, so a swapped `eboot.bin` changes the
  key and still gets verified.
- Do item 1 first; it is independent and shrinks what item 2 has to skip.

### 3. One `patches.py` invocation instead of two — saves ~0.25 s, low priority

`run_windows.py:184` calls `patches.py --print-scaled` and line 201 calls it again for
real. Have the full run print the scaled sizes itself (one extra stdout line parsed by
the launcher). Small win; only worth doing while editing that block for item 2.

## Build configuration (unmeasured; cheap experiments)

### 4. `-fno-omit-frame-pointer` is global in `gpu/CMakeLists.txt:12`

It is applied to the whole GPU library including the shader recompiler and the
rasterizer. On x86-64 it typically costs 1-3 % on call-heavy code. Make it an option
(`BB_FRAME_POINTERS`, default ON to keep Tracy/profiling builds identical) and measure
with the existing `tools/bench.sh` / `BB_FRAME_STATS=1` before flipping the default.

### 5. Silent stale PGO profile

`build.sh` uses `pgo/` automatically whenever a `.gcda` exists, and the `use` build passes
`-Wno-missing-profile -Wno-coverage-mismatch`, so a profile recorded before the engine
hooks, trophies and native menu work is applied silently to code it no longer matches.
A mismatched profile can make things slower than no profile. Two options:

- Cheap: `build.sh` prints the age of the newest `.gcda` next to the "PGO use" line and
  warns when it is older than the newest commit touching `gpu/`.
- Proper: regenerate the profile (`BB_PGO=generate`, play the soak route in
  `tools/soak.sh`, rebuild with `use`) after any large change to `gpu/shim` or `src/`.

## Launcher

### 6. Gamepad enumeration blocks the launcher UI at startup

`launcher/bbport_launcher.py:558` calls `fill_gamepads()` during construction, which runs
`bb-gpu-capabilities --gamepads` synchronously with a 5 s timeout (SDL gamepad init). On a
machine with a slow controller driver the window appears late. The file already has the
async pattern (`communicate_utf8_async`, used for the FSR 4 build at line 853); reuse it
and fill the combo in the callback. Low priority, only user-visible on some machines.

## Inspected, no action recommended

- `src/runtime_mutex.c:47`, `src/runtime_rwlock.c:29`: the spin → `SwitchToThread` →
  `Sleep(1)` ladder is only on the *timed* lock paths, which the comment says are
  uncontended in practice. Untimed locks are plain SRW acquires. Fine.
- `src/runtime_host.c` sleeps: high-resolution waitable timer plus a ≤0.6 ms spin tail.
  Already the right shape for frame pacing.
- `src/runtime_memory.c` region lookups: thread-local 8-entry cache keyed on a table
  generation, read lock only on a miss. Fine.
- `gpu/shim/bbport_copy.cpp`: batch sizes were measured (comment at line 272). Fine.
- Trophies (`src/runtime_services.c:336-370`): in-memory table, append-only file write
  on unlock, no per-frame I/O. Fine.
- Trophy chime (`gpu/shim/bbport_overlay.cpp:582`): synthesises 57 600 samples once per
  toast, about a millisecond. Not worth caching.
- `BbSettings::MenuText`: a branch on a cached enum, not a lookup. Fine.
- Input (`gpu/shim/input/input.cpp`): one small struct copy under a mutex per poll. Fine.
- GPU-side pipeline cache, VMA block sizes, texture GC: already tuned in recent commits
  with A/B toggles (`bbport_toggles.h`). No inspection-level finding; use Tracy or
  `BB_FRAME_STATS=1` if a frame-time regression is suspected.

## Suggested order

1. Item 1 (delete the walk) — one commit, immediate 5.75 s.
2. Item 2 (stamp) with item 3 folded in — one commit, remaining ~6 s.
3. Item 5 cheap variant (stale-profile warning) — one commit.
4. Items 4 and 6 only if someone wants to measure / has the slow-controller symptom.
