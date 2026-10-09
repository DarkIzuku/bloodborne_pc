// SPDX-License-Identifier: GPL-3.0-or-later
// The editor controller/refcount lifecycle is adapted from bbhost, not the GPU renderer.
#include "engine_hooks.h"
#include "bbport_platform.h"
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

extern "C" int runtime_file_translate(const char*, char*, std::size_t);
extern "C" int runtime_file_mount(const char*, const char*);
extern "C" void runtime_file_unmount(const char*);

namespace BbEngine {
namespace {
constexpr std::uint64_t kPreferredGuestSlide = 0x400000;
constexpr std::uint8_t kStepPrologue[] = {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x53,0x50,0x4d,0x89,0xce,0x48,0x89,0xfb};
constexpr std::uint8_t kFramePrologue[] = {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x38};
std::atomic<std::uint64_t> g_made{0};
void Log(const char* format, ...) {
    std::fputs("Engine: ", stdout);
    va_list args; va_start(args, format); std::vprintf(format, args); va_end(args);
    std::putchar('\n');
}
std::int32_t* menu_step_count(std::uint64_t step) { return reinterpret_cast<std::int32_t*>(step + 8); }
void menu_step_hold(std::uint64_t step) { ++*menu_step_count(step); }
void menu_step_release(std::uint64_t step) {
    if (--*menu_step_count(step) == 0) {
        const auto vtable = *reinterpret_cast<std::uint64_t*>(step);
        Call(*reinterpret_cast<std::uint64_t*>(vtable), step);
    }
}
std::uint64_t menu_steps_take(int) { return g_made.exchange(0, std::memory_order_acq_rel); }
bool world_player_block(std::uint32_t* block);

// --- the editor's frame ----------------------------------------------------
//
// The list the talk command opens is character creation's Appearance list
// alone. In character creation it sits in the ChrMake_BG window
// (chrmake_bg.gfx: the book, the panels, the model): its controller
// (sub_1f06b10) makes the preview scenes and their render targets, binds the
// menu's placeholder images to them and, in its update (sub_1f07870), copies
// the hunter's face data into them, swaps them in as each loads, turns the
// mouse and the zoom keys into the model's rotation and distance, and picks
// the image the panel shows. Character creation's top controller (sub_1f8dbb0)
// opens that window from a window descriptor (sub_1ed8150), and its menus
// update it every frame as a dialog's child window (sub_1ed5b10, the base
// update, walks them); at the mirror nothing does either, so the list floats
// over the world with an empty panel and the HUD on top.
//
// Here: the list's window step being made (engine/menu_steps.h: type 7,
// "ChrMakeCommandList") while the hunter is in the world - character creation
// runs at the title, with its own window - opens ChrMake_BG with a descriptor
// of ours, filled as character creation fills its own; each frame its
// controller's update runs; and a reference we hold on the list's step tells
// when the list has closed, which closes the window (sub_1ed8e60).
constexpr std::uint64_t kListType = 7;
constexpr char16_t kListName[] = u"ChrMakeCommandList";
constexpr std::uint64_t kOpenWindow = 0x1ed8150;   // (descriptor)
constexpr std::uint64_t kCloseWindow = 0x1ed8e60;  // (descriptor): closes the window, unloads the movie
constexpr std::uint64_t kMenuHeap = 0x5940418;     // the menus' allocator: +0x58 alloc(size, align), +0x70 free(p)
constexpr std::size_t kDescSize = 0xc0;
constexpr std::uint64_t kDescVtable = 0x5736b30, kDescOwnerVtable = 0x5736b70, kDescListVtable = 0x57310d0;
constexpr std::uint64_t kFactoryVtable = 0x573ee10;  // the functor character creation stores ...
constexpr std::uint64_t kFrameFactory = 0x1f08010;   // ... around this factory: sub_1f06b10's controller
constexpr std::uint64_t kFrameName = 0x4d98486;      // L"ChrMake_BG"
constexpr std::uint64_t kFrameType = 6;

std::uint64_t g_slide = 0;
int g_watch = -1;                          // the list's steps (engine/menu_steps.h)
std::uint64_t g_list = 0;                  // the list's step, while we hold a reference
std::uint64_t g_desc = 0;                  // our descriptor, while ChrMake_BG is open
std::uint8_t g_active = 1;                 // the update's argument (a flag byte it reads)
int g_opened = 0;                          // for the log

std::uint64_t slot(std::uint64_t bn) { return g_slide + (bn - kPreferredGuestSlide); }
std::uint64_t rd64(std::uint64_t a) {
    std::uint64_t v;
    v = 0; BbPlatform::ReadProcessMemory(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(a)), &v, sizeof v);
    return v;
}
void wr64(std::uint64_t a, std::uint64_t v) { std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(a)), &v, sizeof v); }

std::uint64_t heap_alloc(std::size_t n) {
    const std::uint64_t heap = rd64(slot(kMenuHeap));
    if (!heap) return 0;
    return static_cast<std::uint64_t>(Call(rd64(rd64(heap) + 0x58), heap, n, 0x10));
}
void heap_free(std::uint64_t p) {
    const std::uint64_t heap = rd64(slot(kMenuHeap));
    if (heap && p) Call(rd64(rd64(heap) + 0x70), heap, p);
}

bool open_frame() {
    const std::uint64_t d = heap_alloc(kDescSize);
    if (!d) return false;
    std::memset(reinterpret_cast<void*>(static_cast<std::uintptr_t>(d)), 0, kDescSize);
    wr64(d + 0x00, slot(kDescVtable));
    wr64(d + 0x08, slot(kDescOwnerVtable));
    wr64(d + 0x18, kFrameType);
    wr64(d + 0x20, slot(kFrameName));
    wr64(d + 0x30, slot(kFactoryVtable));  // the factory functor, in the descriptor's own storage
    wr64(d + 0x38, slot(kFrameFactory));
    wr64(d + 0x50, d + 0x30);
    wr64(d + 0x68, slot(kDescListVtable));
    Call(slot(kOpenWindow), d);
    if (!rd64(d + 0x60)) {  // no window: nothing to keep
        Call(slot(kCloseWindow), d);
        heap_free(d);
        return false;
    }
    g_desc = d;
    return true;
}

void close_frame() {
    Call(slot(kCloseWindow), g_desc);
    heap_free(g_desc);
    g_desc = 0;
}

void release_list() {
    menu_step_release(g_list);
    g_list = 0;
}

bool g_frame_on = false;

}  // namespace

void change_appearance_tick() {
    if (!g_frame_on) return;
    if (const std::uint64_t made = menu_steps_take(g_watch)) {
        std::uint32_t block = 0;
        if (!g_list && !g_desc && world_player_block(&block) && *menu_step_count(made) >= 1 && open_frame()) {
            menu_step_hold(made);  // ours: when it is the last, the list has closed
            g_list = made;
            if (g_opened++ < 4) Log("change appearance: the editor's frame opened (ChrMake_BG, preview of the hunter)");
        }
    }
    if (g_desc) {
        const std::uint64_t ctl = rd64(g_desc + 0x60);
        if (ctl) Call(rd64(rd64(ctl) + 0x18), ctl, &g_active);
    }
    if (g_list && *menu_step_count(g_list) == 1) {
        close_frame();
        release_list();
    }
}


namespace {
bool world_player_block(std::uint32_t* block) {
    const auto man = rd64(slot(0x593e878));
    const auto chr = man ? rd64(man + 0x60) : 0;
    std::uint32_t value = 0xffffffff;
    if (!chr || !BbPlatform::ReadProcessMemory(reinterpret_cast<void*>(chr + 0x3f8), &value, sizeof value) ||
        value == 0xffffffff) return false;
    if (block) *block = value;
    return true;
}

std::int64_t __attribute__((sysv_abi)) StepHook(std::uint64_t, const std::uint64_t* saved) {
    if (saved[3] != kListType || !saved[2]) return 0;
    char16_t name[sizeof(kListName) / sizeof(char16_t)]{};
    if (BbPlatform::ReadProcessMemory(reinterpret_cast<void*>(saved[2]), name, sizeof name) &&
        std::memcmp(name, kListName, sizeof name) == 0) g_made.store(saved[5], std::memory_order_release);
    return 0;
}
std::int64_t __attribute__((sysv_abi)) FrameHook(std::uint64_t, const std::uint64_t*) {
    change_appearance_tick();
    return 0; // The established FPS++ implementation still runs, byte for byte.
}
} // namespace

void Install(std::uint8_t* image, std::size_t size) {
    const char* asset = std::getenv("BB_DREAM_MIRROR_ASSET");
    const char* identity = std::getenv("BB_ENGINE_IMAGE_SHA256");
    if (!asset || !*asset) return;
    if (!identity || std::strcmp(identity, "071df19c8880086d97182dbc057bc8cb37badaca57d9112683836b24a0444c0a")) {
        Log("mirror disabled: executable identity was not verified"); return;
    }
    constexpr const char* guest = "/app0/dvdroot_ps4/map/mapstudio/m21_00_00_00.msb.dcx";
    const char* original = std::getenv("BB_GAME_DIR");
    char resolved[512]{};
    std::error_code error;
    if (!original || runtime_file_translate(guest, resolved, sizeof resolved) ||
        !std::filesystem::equivalent(resolved, std::filesystem::path(original) / "dvdroot_ps4/map/mapstudio/m21_00_00_00.msb.dcx", error)) {
        Log("mirror disabled: a user mod owns the Dream layout"); return;
    }
    Hook hooks[2];
    if (!Prepare(hooks[0], image, size, 0x1c1cce0, kStepPrologue, StepHook) ||
        !Prepare(hooks[1], image, size, 0x2034770, kFramePrologue, FrameHook)) {
        for (auto& hook : hooks) Discard(hook);
        Log("mirror disabled: engine bytes differ or hook allocation failed"); return;
    }
    if (!std::filesystem::is_regular_file(asset, error) || error || runtime_file_mount(guest, asset)) {
        for (auto& hook : hooks) Discard(hook);
        Log("mirror disabled: prepared layout could not be mounted"); return;
    }
    g_slide = reinterpret_cast<std::uint64_t>(image);
    g_watch = 0;
    g_frame_on = true;
    Commit(hooks);
    Log("Dream mirror enabled: native appearance editor and ChrMake_BG preview; %s", asset);
}
} // namespace BbEngine
