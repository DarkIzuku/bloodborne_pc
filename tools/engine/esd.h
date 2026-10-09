// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from bbhost src/engine/esd.h at 7c790536.
#pragma once

// Talk scripts (script/talk/*.talkesdbnd.dcx: a BND4 of .esd files) as a model
// that edits and writes back.
//
// An .esd is a set of state machines (EzState, long format "fsSL"): state
// groups of states; a state's entry, exit and while commands; its conditions,
// each an expression (bytecode), a target state, pass commands and nested
// conditions. Offsets are relative to the data block at 0x6c and the data is
// laid out section by section: header (0x48), groups (0x20), states (0x48),
// conditions (0x38), commands (0x18), command args (0x10), condition-pointer
// lists (8), expression bytecode, the script's name (UTF-16, 2-aligned).
//
// Two things the game's own writer does, kept for byte-identical output: each
// group's states are followed by a copy of its initial state (same id, own
// condition list, not counted in the group), and each group's initial-state
// condition list is written twice, the second copy unreferenced.
//
// Every element remembers the offset it was read from (its key) and is written
// back in key order, so parse then serialize gives the input bytes again - all
// 271 talk scripts in the 1.09 dump do (tests/esd_test). Elements added by an
// edit have no key and follow the originals of their section.

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace esd {

constexpr std::int64_t kNew = std::numeric_limits<std::int64_t>::max();  // the key of an added element

struct Expr {
    std::vector<std::uint8_t> code;
    std::int64_t key = kNew;
};

// A contiguous list in its section (commands, args, condition pointers): indices
// into the matching pool. An empty run writes the offset it was read with.
struct Run {
    std::vector<int> items;
    std::int64_t key = kNew;
    std::int64_t empty_off = -1;
};

struct Cmd {
    std::int32_t bank = 1, id = 0;
    int args = -1;  // a Run of Exprs
};

struct Cond {
    int target = -1;  // a State, or -1
    int eval = -1;    // an Expr
    int pass = -1;    // a Run of Cmds
    int subs = -1;    // a Run of Conds
    std::int64_t key = kNew;
};

struct State {
    std::int64_t id = 0;
    int conds = -1, entry = -1, exit = -1, whiles = -1;  // Runs
};

struct Group {
    std::int64_t id = 0;
    std::vector<int> states;
    int copy = -1;  // the initial state's copy the game's files carry after the group's states
    std::int64_t key = kNew;
};

struct Script {
    std::vector<std::uint8_t> head, dhead, dtail, name;
    std::int64_t empty_groups_off = -1;
    std::vector<Expr> exprs;
    std::vector<Run> runs;
    std::vector<Cmd> cmds;
    std::vector<Cond> conds;
    std::vector<State> states;
    std::vector<Group> groups;
    std::vector<int> dead;  // condition-pointer Runs nothing references

    // Builders for edits: each returns the new element's index.
    int expr(std::vector<std::uint8_t> code);
    int run(std::vector<int> items);
    int cmd(std::int32_t bank, std::int32_t id, const std::vector<std::vector<std::uint8_t>>& args);
    int cond(int target, std::vector<std::uint8_t> eval, int pass = -1, int subs = -1);
    int state(std::int64_t id);  // with empty lists
    Group* group(std::int64_t id);
    int state_in(const Group& g, std::int64_t id) const;  // -1 when the group has none
};

// False with the reason when the bytes are not a long-format talk script.
bool parse(const std::vector<std::uint8_t>& bytes, Script& out, std::string* why);
std::vector<std::uint8_t> serialize(const Script& s);

// Expression bytecode, as the game's own conditions and arguments are written.
std::vector<std::uint8_t> lit(std::int64_t v);  // an integer
std::vector<std::uint8_t> call(int fn, const std::vector<std::int64_t>& args);  // a talk function's result
std::vector<std::uint8_t> done(std::vector<std::uint8_t> code);  // the end marker appended

}  // namespace esd
