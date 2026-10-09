// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from bbhost src/engine/rebirth_script.h at 7c790536.
#pragma once

// The Altar of Despair's talk script with "Rebirth in the Nightmare".
//
// t242307 (script/talk/m24_02_00_00.talkesdbnd.dcx) is the altar's menu:
// "Offer Flesh To Altar" (with Queenly Flesh) and "Do Nothing". This adds a
// third entry ahead of "Do Nothing" and its states (engine/esd.h):
//
//   - without the Yharnam Stone (goods 4330): a message, and the talk ends;
//   - with it: kReqReset is set and the script waits for the host to clear it
//     (engine/rebirth.cpp resets the hunter to their origin's level and stats
//     and refunds the echoes; kFail set means it could not, with a message);
//   - the doll's level-up menu (talk command 31) opens; kMenu set before it is
//     cleared by the host when the menu has closed, because the doll's own
//     check - function 25, "menu 23 is open" - never reads 1 at the altar;
//   - then a choice: accept (the talk ends) or undo (kReqUndo, which the
//     host answers by restoring the hunter as they were); closing the choice
//     opens the level-up menu again.
//
// The handshake is the altar's own: "Offer Flesh" sets 72500326 and waits for
// the event script to clear it. The four flags are unused by the game's event
// and talk scripts; the host clears them at every world load.
//
// The input is the 1.09 dump's script and the output a known one: both are
// checked by SHA-256.

#include <cstdint>
#include <string>
#include <vector>

namespace rebirth {

constexpr std::uint32_t kReqReset = 72500390, kReqUndo = 72500391, kFail = 72500392, kMenu = 72500393;
constexpr std::int32_t kStone = 4330;  // goods: the Yharnam Stone
// Event texts (group 30), the port's own (tools/pc_option_messages.tsv).
constexpr std::int32_t kTextOption = 14000332, kTextNoStone = 14000333, kTextFail = 14000334, kTextAccept = 14000335,
                       kTextUndo = 14000336;

}  // namespace rebirth

// esd: the decompressed t242307.esd. True with it rewritten; false with the
// reason, esd untouched, when it is not the 1.09 script.
bool rebirth_script(std::vector<std::uint8_t>& esd, std::string* why);
