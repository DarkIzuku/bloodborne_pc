# Renderer performance and lifetime fixes

Base: public `410f8ced95f1e69a8c5b04630c2acc0df947a161`.
Integration branch: `public-performance-stability-v2`.

* Release nested constant-engine, graphics and compute PM4 coroutine frames after
  their final suspension, retaining top-level ownership and ordering. This is a CPU
  allocation leak fix, not a claim about VRAM or a measured FPS percentage.
* Avoid repeated synchronous tiled-texture writebacks during ordinary memory
  pressure. A target stays resident until unused for a minute; critical pressure
  retains completed-use guards and bounded, lossless emergency writeback.
  The existing dynamic 80% pressure policy remains; no unrelated reserved arena
  is imported. `BB_GC_EAGER_WRITEBACK=1` restores the previous timing for diagnosis.
  `BB_IMAGE_WRITEBACK_GC=0` retains its existing meaning.
* Apply EarlyFragmentTests only for the guest's DEPTH_BEFORE_SHADER request, with
  late tests retained for depth/stencil exports and emulated clipping. Shader
  specialization includes this state. `BB_EARLY_Z=0` is a diagnostic startup opt-out.
  RuntimeInfo metadata advances to schema 8: older metadata rebuilds once; the
  resulting cache persists on subsequent starts. Driver-cache validation remains.
* Record TileImage staging-buffer reads so reuse cannot overwrite an in-flight
  tiling input. The existing deferred-recording architecture remains unchanged.

Sources are pinned in `licenses/PERFORMANCE-STABILITY-V2.md`. Tests exercise actual
PM4 decoders, shader emission and serialization, collector policy, and both Vulkan
tiling paths. Hosted CPU tests and local GPU readback are reported separately.
Visual gameplay, long-session memory and comparative FPS need real playtesting;
synthetic results do not establish a universal performance gain. Frame Generation
is not included.

Local focused checks passed: 64 SPIRV-Tools shader validations, motion shader/cache
checks and actual tiling readback in async, sync and direct recording modes.
The Python regression suite passed 95 tests with one skip. The shared decoder
regression detects the previous leak and passes with the releases restored.
Standalone renderer targets now inherit the OneCore library needed for Windows
file mappings; this corrects a pre-existing test-link omission without runtime
logic changes. Full build and hosted checks are reported with the release.

The optional GPU regression executables now use the production Vulkan-Hpp ABI
(`NDEBUG`) with independent test assertions. Mixing debug dispatch-table layouts
with the release library caused a null function-pointer call in the camera test;
this test-only mismatch is corrected without changing the production renderer.
Camera/object motion, scene-proxy resolution, TAA/RCAS and all 55 NGX model/quality
combinations are exercised in three NGX recording modes using synthetic images.
