# Windows stability integration

Based on the public Windows branch at `8ecba9a4abdd54747e31d7fee54982e422b5f8e2`.
Changes are ported individually, without replacing the renderer or launcher.

* Deferred Vulkan recording keeps its 128 KiB fast path and owns aligned overflow
  allocations until recording finishes, including command-chunk recycling.
* Texture collection observes GPU completion and current budgets. Eligible tiled
  colour textures are written back before eviction. Untested macro-tiled mip
  chains, depth/stencil, MSAA, volumes and conflicting writes stay resident.
  `BB_IMAGE_WRITEBACK_GC=0` disables the additional writeback path for diagnosis.
* Null and incomplete vertex descriptor tables are rejected before dereferencing.
* Byte-verified 1.09 render-pool and red-zone patches protect transient allocations
  and Windows fault-handler stack state. `BB_RENDER_POOLS=0` and `BB_RED_ZONE=0`
  disable their respective changes. Original game files are never modified.
* Runtime memory callbacks preserve order after partial unmapping and support
  growing queues. Audio allocations and save enumeration check size/allocation
  failures without changing save formats.
* Bounded Windows crash dumps retain exception context, modules and selected
  memory; fatal renderer assertions capture diagnostics before terminating.
  `BB_CRASH_DUMP=0` disables dumps. Sensitive registered regions are excluded.
* DLSS Model Preset is independent of Quality/Balanced/Performance/DLAA, stored as
  `dlss_preset` in `bbport.ini`. Auto preserves the existing NVIDIA defaults.
  Unsupported hints fall back safely. Model changes wait for GPU completion,
  recreate NGX state and reset history. Older model hints may be remapped by the
  installed NVIDIA DLL. The packaged runtime remains DLSS 310.9.1.
* SHA-256 verified executable preparation uses atomic staging and complete-output
  validation. Executable/module/preparation changes invalidate the cache. Mods,
  selected XML patches and local engine assets are processed on every launch.
  `BB_PREPARE_CACHE=0` forces rebuilding. No game-derived cache is distributed.
* SDL controller discovery retains explicit user overrides, retries initialization
  and preserves a working controller when a replacement cannot be opened.
* Public PR #1 adds Quit Game to the title menu using the original confirmation
  dialog. PR #2 adds local trophies, a notification banner, a native System page
  and an overlay list. Unlocks persist in the runtime user directory's
  `trophies.txt`; base-game platinum excludes DLC trophies. Unrelated launcher
  redesign and default-path changes from the PR were not imported.

The original loading timers are retained. Frame Generation and unlimited FPS
are not introduced. Existing online configuration and rendering features remain.

## Verification

The Windows workflow builds the runtime and WPF launcher, runs preparation/mod
safety suites, native recording ownership and descriptor/memory regressions,
trophy persistence/concurrency tests, crash injection and launcher round trips.
Hosted runners cannot validate NGX dispatch or Vulkan texture readback; those
require a compatible local GPU. Long gameplay sessions, Intel-specific crash
reproduction and visual trophy/menu checks remain separate from synthetic tests.

Sources: AwesomeObserver/windows (NGX, recording, crash and preparation work),
Awakenchan and Supermedo (focused stability adaptations), and GerasimosKan's
public PRs #1 and #2. See the license notices for attribution and upstream links.
