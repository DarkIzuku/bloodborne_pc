// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from bbhost src/engine/rebirth_script.cpp at 7c790536.
#include "rebirth_script.h"

#include "sha256.h"
#include "esd.h"

#include <cstring>
#include <initializer_list>

namespace {

constexpr const char* kInputSha256 = "e56163a1bb8f46ae59368160e522e11f970e2268f6d356b823e4d72a7c969ff1";
constexpr const char* kOutputSha256 = "e726dec68e8d73c5d5b6ba1875fabd6a85320b6b14f95ca3d6729143a3c362b2";
constexpr std::int64_t kBuildGroup = 2147483640;   // fills the talk list
constexpr std::int64_t kHandleGroup = 2147483641;  // shows it and acts on the choice

using Code = std::vector<std::uint8_t>;

Code cat(std::initializer_list<Code> parts) {
    Code out;
    for (const Code& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}
const Code kEq{0x95}, kReg0{0xaf};
Code eq(const Code& lhs, std::int64_t rhs) { return esd::done(cat({lhs, esd::lit(rhs), kEq})); }
Code arg(std::int64_t v) { return esd::done(esd::lit(v)); }

}  // namespace

bool rebirth_script(std::vector<std::uint8_t>& bytes, std::string* why) {
    const auto fail = [&](const char* reason) {
        if (why) *why = reason;
        return false;
    };
    if (sha256_hex(bytes.data(), bytes.size()) != kInputSha256) return fail("not the 1.09 t242307 (the Altar of Despair)");
    esd::Script s;
    if (!esd::parse(bytes, s, why)) return false;
    esd::Group* build = s.group(kBuildGroup);
    esd::Group* handle = s.group(kHandleGroup);
    if (!build || !handle) return fail("the altar's menu groups are missing");
    const int list_rest = s.state_in(*build, 3), choice = s.state_in(*handle, 1), end = s.state_in(*handle, 6);
    if (list_rest < 0 || choice < 0 || end < 0) return fail("the altar's menu states are missing");
    auto cmd = [&](std::int32_t id, std::initializer_list<std::int64_t> args) {
        std::vector<Code> a;
        for (const std::int64_t v : args) a.push_back(arg(v));
        return s.cmd(1, id, a);
    };
    using namespace rebirth;

    // The entry, ahead of "Do Nothing".
    {
        std::vector<int> items{cmd(19, {3, kTextOption, -1})};
        const auto& old = s.runs[static_cast<std::size_t>(s.states[static_cast<std::size_t>(list_rest)].entry)].items;
        items.insert(items.end(), old.begin(), old.end());
        s.states[static_cast<std::size_t>(list_rest)].entry = s.run(items);
    }
    // The new states, 7..15 of the handling group.
    int st[16];
    for (int id = 7; id <= 15; ++id) {
        st[id] = s.state(id);
        handle = s.group(kHandleGroup);
        handle->states.push_back(st[id]);
    }
    auto set = [&](int state, int conds, int entry, int whiles) {
        esd::State& x = s.states[static_cast<std::size_t>(state)];
        if (conds >= 0) x.conds = conds;
        if (entry >= 0) x.entry = entry;
        if (whiles >= 0) x.whiles = whiles;
    };
    Code wait_arg{0x81, 0, 0, 0, 0, 0, 0, 0, 0, 0xa1};  // the altar's own while-command, c1_68(0.33)
    const double secs = 0.33;
    std::memcpy(wait_arg.data() + 1, &secs, 8);
    const int wait = s.cmd(1, 68, {wait_arg});
    const Code truth = esd::done(esd::lit(1));
    const Code dialog_closed = eq(esd::call(58, {0}), 0);

    // Chosen: the stone, or a message.
    set(st[7], s.run({s.cond(st[9], eq(esd::call(47, {3, kStone, 5, 0, 0}), 1)), s.cond(st[8], truth)}), -1, -1);
    set(st[8], s.run({s.cond(end, dialog_closed)}), s.run({cmd(17, {7, kTextNoStone, 1, 0, 1})}), -1);
    // The host's reset.
    set(st[9], s.run({s.cond(st[10], eq(esd::call(15, {kReqReset}), 0))}), s.run({cmd(11, {kFail, 0}), cmd(11, {kReqReset, 1})}), s.run({wait}));
    set(st[10], s.run({s.cond(st[11], eq(esd::call(15, {kFail}), 1)), s.cond(st[12], truth)}), -1, -1);
    set(st[11], s.run({s.cond(end, dialog_closed)}), s.run({cmd(11, {kFail, 0}), cmd(17, {7, kTextFail, 1, 0, 1})}), -1);
    // The level-up menu, until the host says it has closed.
    set(st[12], s.run({s.cond(st[13], eq(esd::call(15, {kMenu}), 0))}), s.run({cmd(11, {kMenu, 1}), s.cmd(1, 31, {})}), -1);
    // Leaving it: accept or undo; closing the choice goes back to the menu.
    set(st[13], s.run({s.cond(st[14], truth)}), s.run({s.cmd(1, 20, {}), cmd(19, {1, kTextAccept, -1}), cmd(19, {2, kTextUndo, -1})}), -1);
    set(st[14],
        s.run({s.cond(end, esd::done(cat({esd::lit(23), Code{0x84, 0xa7}, esd::lit(1), kEq}))),  // reg0 = f23(); reg0 == 1: accept
               s.cond(st[15], eq(kReg0, 2)),                                                     // undo
               s.cond(st[12], eq(esd::call(53, {}), 0))}),                                       // the choice closed: back to the menu
        s.run({s.cmd(1, 76, {})}), s.run({wait}));
    set(st[15], s.run({s.cond(end, eq(esd::call(15, {kReqUndo}), 0))}), s.run({cmd(11, {kReqUndo, 1})}), s.run({wait}));

    // The choice: entry 3 goes to the new states, ahead of "Do Nothing / closed".
    {
        const auto old = s.runs[static_cast<std::size_t>(s.states[static_cast<std::size_t>(choice)].conds)].items;
        std::vector<int> items{old.front(), s.cond(st[7], eq(kReg0, 3))};
        items.insert(items.end(), old.begin() + 1, old.end());
        s.states[static_cast<std::size_t>(choice)].conds = s.run(items);
    }
    std::vector<std::uint8_t> out = esd::serialize(s);
    if (sha256_hex(out.data(), out.size()) != kOutputSha256) return fail("the rewrite is not the known result");
    bytes = std::move(out);
    return true;
}
