# Public contributions: October 22, 2026

Base: public-performance-stability-v2 at c1fe027. Contributions reviewed at
PR #3 308211327b4fd6d0924e6bd038f4a56479cb8a56,
PR #4 639a6544b6e7aa6e8ad1b7ce879354061f571e85,
PR #5 941dec22ec6e0a447306855d882fb1dbe5f64838.

These pull requests target a different master and overlap. Their full upstream
history is deliberately not merged into the Windows release branch.

* PR #3's trophies are already present through PR #2 (5fb10f7); the tested
  Windows xbyak dependency fallback is already present. Reimporting its newer
  renderer and unrelated upstream changes would not be a trophy fix.
* PR #4's content-recognized button prompts and controller cursor behavior are
  adapted. Keyboard/mouse and Xbox labels are generated from our bundled font;
  no extracted game or third-party texture blob is added. PlayStation and
  unrecognized/modded textures remain unchanged. Replacement buffers are owned
  by each calling thread, and observed resource addresses are bounded.
* PR #5 adds demand-committed Windows guest memory, atomic launcher settings
  saves, and individually selectable XML/executable mods. The ELF loader checks
  format, table bounds, segment overlap/layout, maximum image/patch size and
  write count. Identically named XML files in different mods have distinct keys.
  Windows mod junctions are rejected as symlinks already were.

Preserved deliberately: arbitrary user-selected game folders, existing WPF
layout/artwork, FIFO for 120/Unlimited, crash diagnostics, hash-verified executable
cache and per-launch processing of content/menu/patch layers. The proposed
size/mtime preparation stamp is weaker than our current cache and is not used.
The PR removes only the launcher's FIFO lock; it has no matching Vulkan/frame
pacing improvement, so that removal is not adopted. No new NGX model or DLSS
renderer changes exist in PR #5 relative to c1fe027.

The Windows pool reserves the same section size but commits allocated ranges,
using CreateFileMapping(SEC_RESERVE) and VirtualAlloc(MEM_COMMIT). This reduces
initial commit charge, not the configured pool size or promised gameplay FPS.
Committed section pages are retained for reuse after release.
Reference: https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-createfilemappinga

Validation before packaging: native C++ tests, real Windows section allocation,
alias/write/release/reuse tests, malformed executable/XML/mod tests, procedural
glyph generation and WPF settings/lifecycle tests. In-game visual quality,
controller-specific artwork and end-to-end third-party executable mods still
require gameplay testing. Synthetic tests do not demonstrate higher FPS.
