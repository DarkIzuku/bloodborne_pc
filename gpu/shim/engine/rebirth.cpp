// SPDX-License-Identifier: GPL-3.0-or-later
// Stat application, origin refund and native menu lifecycle from bbhost rebirth.cpp.
#include "rebirth.h"
#include "engine_state.h"
#include "rebirth_refund.h"
#include "../../../tools/engine/rebirth_script.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
extern "C" int runtime_file_translate(const char*,char*,std::size_t);
extern "C" int runtime_file_mount(const char*,const char*);
extern "C" void runtime_file_unmount(const char*);
namespace BbEngine::Rebirth {
namespace fs = std::filesystem;

namespace {

constexpr const char* kArchive = "dvdroot_ps4/script/talk/m24_02_00_00.talkesdbnd.dcx";
constexpr std::uint32_t kAltarBlock = 0x18020000;  // m24_02_00_00: the Altar of Despair's map
constexpr std::uint64_t kApplyStats = 0x2013d00;   // the level-up's apply (the YES of "Spend ... Blood Echoes")
constexpr std::uint64_t kWorldChrMan = 0x593e878;  // main player ChrIns at +0x60
constexpr std::int64_t kEchoCap = 999999999;       // the game's own limit
constexpr int kMenuWaitFrames = 120;               // the level-up menu not open by then: give up on it
constexpr const char* kStats[6] = {"vitality", "endurance", "strength", "skill", "bloodtinge", "arcane"};

bool g_on = false;
std::uint64_t g_slide = 0;
int g_watch = -1;          // the level-up menu's steps (engine/menu_steps.h)
bool g_loading = true;     // no world since the last check: the flags are cleared when it comes back
bool g_have_snapshot = false;
Build g_snapshot;          // the hunter before the rebirth, for "Undo Rebirth"
std::uint64_t g_menu = 0;  // the level-up menu's step, while we hold a reference
int g_menu_wait = 0;
int g_said = 0;

std::uint64_t slot(std::uint64_t bn) { return g_slide + (bn - kPreferredGuestSlide); }
void* code(std::uint64_t a) { return reinterpret_cast<void*>(static_cast<std::uintptr_t>(a)); }
template <typename T>
bool rd(std::uint64_t at, T* out) {
    return host_read_safe(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(at)), out, sizeof(T));
}

bool read_build(Build& b) {
    bool ok = player_stat_get("level", &b.level) && player_stat_get("echoes", &b.echoes);
    for (int i = 0; i < 6; ++i) ok = ok && player_stat_get(kStats[i], &b.stat[i]);
    return ok;
}

// The game's own apply, which the level-up's YES runs: the attributes and level
// into the record, then hit points, stamina and the character refreshed. It
// reads them from a functor's captures: +8 the level, +0xc.. the six.
void apply(const Build& b) {
    alignas(16) static std::uint32_t captures[10];
    std::memset(captures, 0, sizeof(captures));
    captures[2] = static_cast<std::uint32_t>(b.level);
    for (int i = 0; i < 6; ++i) captures[3 + i] = static_cast<std::uint32_t>(b.stat[i]);
    hle_call_guest<std::int64_t>(code(slot(kApplyStats)), reinterpret_cast<std::uint64_t>(captures));
}

// Lowering vitality, the apply takes the drop in maximum hit points off the
// current ones, which can leave the hunter at 0 - dead at the altar. Rebirth
// leaves them whole instead: the character's hit points and the record's.
void heal() {
    std::uint64_t man = 0, chr = 0, mods = 0, hp = 0;
    std::int32_t max = 0;
    if (!rd(slot(kWorldChrMan), &man) || !man || !rd(man + 0x60, &chr) || !chr || !rd(chr + 0x3b0, &mods) || !mods ||
        !rd(mods + 0x20, &hp) || !hp || !rd(hp + 0xfc, &max) || max <= 0)
        return;
    std::memcpy(code(hp + 0xf8), &max, sizeof max);
    player_stat_set("hp", max);
}

// The echoes for one level, as the level-up screen prices it (sub_1f2f5e0):
// CalcCorrectGraph row 200's init_inclination_soul, adjustment_value,
// boundry_inclination_soul and boundry_value at +0x3c..+0x48.
bool reset() {
    Build cur;
    int origin = -1;
    if (!read_build(cur) || !player_origin(&origin)) {
        host_log("rebirth: no character to read");
        return false;
    }
    if (cur.level<4 || cur.level>544 || cur.echoes<0 || cur.echoes>kEchoCap) return false;
    for (auto s:cur.stat) if (s<1 || s>99) return false;
    std::size_t n = 0, gn = 0;
    const auto* row = static_cast<const std::uint8_t*>(params_row("CharaInitParam", 3000u + static_cast<std::uint32_t>(origin), &n));
    const auto* graph = static_cast<const std::uint8_t*>(params_row("CalcCorrectGraph", 200, &gn));
    if (!row || n < 0xc9 || !graph || gn < 0x4c) {
        host_log("rebirth: no origin row (origin %d) or level-cost row", origin);
        return false;
    }
    Build base;
    std::int16_t lv = 0;
    std::memcpy(&lv, row + 0xc0, 2);
    base.level = lv;
    const std::size_t at[6] = {0xc2, 0xc3, 0xc5, 0xc6, 0xc7, 0xc8};  // baseVit, baseWil, baseStr, baseDex, baseMag, baseFai
    for (int i = 0; i < 6; ++i) base.stat[i] = row[at[i]];
    float g[4];
    std::memcpy(g, graph + 0x3c, sizeof g);
    std::int64_t refund=0;
    if (!Refund(cur,base,g,&refund)) { host_log("rebirth: unsafe stats, graph or echo refund rejected");return false; }
    g_snapshot = cur;
    g_have_snapshot = true;
    base.echoes = cur.echoes + refund;
    apply(base);
    heal();
    player_stat_set("echoes", base.echoes);
    host_log("rebirth: level %lld to %lld (origin %d), %lld blood echoes given back, %lld held", static_cast<long long>(cur.level),
             static_cast<long long>(base.level), origin, static_cast<long long>(refund), static_cast<long long>(base.echoes));
    return true;
}

void undo() {
    if (!g_have_snapshot) {
        host_log("rebirth: nothing to undo");
        return;
    }
    apply(g_snapshot);
    heal();
    player_stat_set("echoes", g_snapshot.echoes);
    g_have_snapshot = false;
    host_log("rebirth: undone - level %lld, %lld echoes", static_cast<long long>(g_snapshot.level), static_cast<long long>(g_snapshot.echoes));
}

void set_flag(std::uint32_t id, bool v) { event_flag_set(id, v); }
bool flag(std::uint32_t id) {
    bool v = false;
    return event_flag_get(id, &v) && v;
}

}  // namespace

void Tick() {
    if (!g_on) return;
    const std::uint64_t made = menu_steps_take(g_watch);  // every frame: a doll's level-up is not ours
    std::uint32_t block = 0;
    if (!world_player_block(&block)) {
        g_loading = true;
        return;
    }
    using namespace rebirth;
    if (g_loading) {  // a world came (back): a request a save carried over is not one
        g_loading = false;
        for (const std::uint32_t id : {kReqReset, kReqUndo, kFail, kMenu}) set_flag(id, false);
        g_have_snapshot = false;
        if (g_menu) menu_step_release(g_menu), g_menu = 0;
        return;
    }
    if (flag(kReqReset)) {
        if (block != kAltarBlock || !reset()) set_flag(kFail, true);
        set_flag(kReqReset, false);
    }
    if (flag(kReqUndo)) {
        undo();
        set_flag(kReqUndo, false);
    }
    if (flag(kMenu)) {
        if (!g_menu && made && *menu_step_count(made) >= 1) {
            menu_step_hold(made);  // ours: when it is the last, the menu has closed
            g_menu = made;
            g_menu_wait = 0;
        }
        if (g_menu && *menu_step_count(g_menu) == 1) {
            menu_step_release(g_menu);
            g_menu = 0;
            set_flag(kMenu, false);
        } else if (!g_menu && ++g_menu_wait > kMenuWaitFrames) {
            g_menu_wait = 0;
            set_flag(kMenu, false);
            if (g_said++ < 4) host_log("rebirth: the level-up menu did not open");
        }
    } else if (g_menu) {
        menu_step_release(g_menu);
        g_menu = 0;
    }
}


bool Install(std::uint8_t* image,std::size_t size) {
    const char* asset=std::getenv("BB_REBIRTH_ASSET"),*game=std::getenv("BB_GAME_DIR");
    if (!asset || !*asset || !game || !*game || !std::getenv("BB_PC_MENU_ASSETS")) return false;
    // The native apply method must still have the verified 1.09 code.
    constexpr std::size_t at=0x1c13d00;
    constexpr std::uint8_t prologue[]={0x55,0x48,0x89,0xe5};
    if (size<at+sizeof prologue || std::memcmp(image+at,prologue,sizeof prologue)) return false;
    char resolved[512]{}; std::error_code ec;
    const std::string guest=std::string("/app0/")+kArchive;
    if (runtime_file_translate(guest.c_str(),resolved,sizeof resolved) ||
        !fs::equivalent(fs::u8path(resolved),fs::u8path(game)/kArchive,ec) || ec ||
        !fs::is_regular_file(fs::u8path(asset)) || runtime_file_mount(guest.c_str(),asset)) return false;
    g_slide=reinterpret_cast<std::uint64_t>(image);
    g_watch=menu_steps_watch(7,u"LevelUp");
    if (g_watch<0) { runtime_file_unmount(guest.c_str());return false; }
    g_on=true;
    host_log("rebirth: Altar of Despair uses bbhost's native respec with Yharnam Stone, refund and undo");
    return true;
}
}
