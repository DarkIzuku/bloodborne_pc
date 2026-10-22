// SPDX-License-Identifier: GPL-3.0-or-later
// Button prompts for the device in use, as Elden Ring shows them: the game's own PlayStation
// glyphs for a DualShock or DualSense, procedural Xbox labels for any other controller, and keycaps and mouse buttons drawn from the key bindings while the keyboard
// and mouse play (PC controls). The game's textures are swapped as the texture cache uploads them
// - the key guide's KG_* glyphs and the HUD's in MENU_Common_00091 of menu/common.tpf.dcx,
// recognised by their contents - so no game file changes, and a mod's own glyphs stay its own.
#pragma once
#include <cstdint>

union SDL_Event;

namespace BbPrompts {
/// Window thread, every SDL event: notes the device in use; a change re-uploads the glyphs.
void Event(const SDL_Event& event);
/// Texture cache, about to upload `size` bytes of guest memory at `address` into an image: the
/// bytes to upload instead (same size and tiling), or null for the guest's own. Valid until the
/// next call.
const std::uint8_t* Replace(std::uint64_t address, std::uint64_t size);
} // namespace BbPrompts
