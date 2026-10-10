// SPDX-License-Identifier: GPL-3.0-or-later
// Native System page and guest row builders adapted from bbhost option_menu.cpp.
// The existing renderer and bbport.ini remain authoritative.
#include "option_menu.h"
#include "engine_hooks.h"
#include "menu_memory.h"
#include "graphics.h"
#include "../input/bindings.h"
extern bool debug_menu_active();
extern std::uint32_t menu_confirm_button();
extern std::uint32_t menu_back_button();
extern std::uint32_t input_delivered_buttons();
#include <chrono>
#include <SDL3/SDL.h>
#include "bbport_platform.h"
#include "bbport_settings.h"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <vector>
extern "C" int runtime_file_translate(const char*,char*,std::size_t);
extern "C" int runtime_file_mount(const char*,const char*);
extern "C" void runtime_file_unmount(const char*);
extern "C" int runtime_trophy(int,const char**,const char**,int*,int*,std::int64_t*); // runtime_services.c
namespace BbEngine::Options {
namespace {
#define GUEST_ABI __attribute__((sysv_abi))
std::uint64_t g_slide=0;
bool g_installed=false;
constexpr std::uint64_t kPreferredGuestSlide=0x400000;
constexpr std::uint64_t kOpenSection=0x1f20900,kScratchInit=0x208af20,kAddSliderRow=0x1f2ac00;
constexpr std::uint64_t kAddChoiceRow=0x1f2a100,kBuildOnOff=0x1f2b3b0,kWstrCpy=0x2d67040;
constexpr std::uint64_t kListAppend=0x1f1c0c0,kAddListRow=0x1f29370;
constexpr std::uint64_t kListRedraw=0x1ed06e0,kLayoutRows=0x1f28de0,kMsgRepository=0x1ee8cc0;
constexpr std::uint32_t kDefaultsRowId=0x9d8a,kMsgMenuText=200,kMsgLineHelp=201;
constexpr std::uint64_t kAddCommandRow=0x1f4e2a0,kSystemFinalize=0x1fea630;
constexpr std::uint64_t kOpenSectionFlow=0x1fb4c70,kRowFunctorVTable=0x573d120,kOpenerFunctorVTable=0x5743880;
constexpr int kSectionSlots=6;
char g_open_name[32]="PCCamera";
std::uint64_t guest(std::uint64_t bn) { return g_slide+(bn-kPreferredGuestSlide); }
void* guest_fn(std::uint64_t bn) { return reinterpret_cast<void*>(guest(bn)); }
template<typename T,typename... A> T hle_call_guest(void* fn,A... a) { return static_cast<T>(Call(reinterpret_cast<std::uint64_t>(fn),a...)); }
void host_log(const char* format,...) {
    std::fputs("Engine: ",stdout); va_list args; va_start(args,format); std::vprintf(format,args); va_end(args); std::putchar('\n');
}
struct GuestFunction {
    alignas(16) std::uint8_t buf[0x28]{};

    GuestFunction(std::uint64_t vtable_bn, void* fn) {
        const std::uint64_t vt = guest(vtable_bn);
        void* self = &buf[0];
        std::memcpy(&buf[0x00], &vt, sizeof(vt));
        std::memcpy(&buf[0x08], &fn, sizeof(fn));
        std::memcpy(&buf[0x20], &self, sizeof(self));
    }
};

// A caption pair: two objects 0x40 apart, the label in the first and the line
// help in the second, which is the shape every row builder reads. Within each,
// +0x00 is the raw UTF-16 pointer the row draws, +0x08 a wstring the command
// row copies, +0x38 a flag.
//
// The port used to build one of these from an arbitrary message id and then
// store a pointer to its own string literal over the text. It drew, but the
// flags beside it were the placeholder's, and a section title - which the
// movie carries as a `StaticText_<id>` placement, the instance name being the
// id - could not be done that way at all. So the port has message ids of its
// own now (tools/pc_option_messages.tsv, written into the overlay's
// menu.msgbnd.dcx by engine/menu_assets.cpp) and asks for them exactly as the game
// does.
struct Captions {
    alignas(16) std::uint8_t buf[0x80]{};

    explicit Captions(std::uint32_t id) {
        hle_call_guest<std::int64_t>(guest_fn(kMsgRepository), &buf[0], kMsgMenuText, id);
        hle_call_guest<std::int64_t>(guest_fn(kMsgRepository), &buf[0x40], kMsgLineHelp, id);
    }

    // The repository's text is copied into a wstring beside the pointer, and a
    // caption long enough to leave the SSO buffer put it on the heap. Every
    // builder that makes one of these frees it the same way, so this does too:
    // one leak per menu open is still a leak.
    ~Captions() {
        free_wstring(&buf[0x08]);
        free_wstring(&buf[0x48]);
    }

    Captions(const Captions&) = delete;
    Captions& operator=(const Captions&) = delete;

    // MSVC's _String_val: the union at +0x00 is either eight wchars or the
    // heap pointer, the length is at +0x10, the capacity at +0x18 and the DL
    // allocator at +0x20. Capacity below 8 is the small-string buffer.
    static void free_wstring(void* ws) {
        auto* p = static_cast<std::uint8_t*>(ws);
        std::uint64_t cap = 0, ptr = 0, alloc = 0;
        std::memcpy(&cap, p + 0x18, sizeof(cap));
        if (cap < 8) {
            return;
        }
        std::memcpy(&ptr, p, sizeof(ptr));
        std::memcpy(&alloc, p + 0x20, sizeof(alloc));
        if (!ptr || !alloc) {
            return;
        }
        std::uint64_t vtable = 0;
        std::memcpy(&vtable, reinterpret_cast<void*>(static_cast<std::uintptr_t>(alloc)),
                    sizeof(vtable));
        std::uint64_t fn = 0;
        std::memcpy(&fn, reinterpret_cast<void*>(static_cast<std::uintptr_t>(vtable + 0x70)),
                    sizeof(fn));
        hle_call_guest<std::int64_t>(reinterpret_cast<void*>(static_cast<std::uintptr_t>(fn)),
                                     alloc, ptr);
    }
};

// The repository's own text for a message id: the caption object's +0x00,
// which the repository owns and which outlives anything the port holds.
const char16_t* message_text(std::uint32_t id) {
    Captions c(id);
    const char16_t* t = nullptr;
    std::memcpy(&t, &c.buf[0], sizeof(t));
    return t;
}

// A list entry, the 0x48 bytes every list the rows take is made of: the
// value at +0x00 (a byte in a choice row's list, an int32 in a pick list's),
// the raw UTF-16 pointer that is drawn at +0x08, a wstring copy of it at
// +0x10 and a flag at +0x40. The containers deep-copy an entry, so the
// caller's copy, and later the container's, own a heap buffer for any text
// of eight characters or more, and free it the way the game's own handlers
// do: the buffer pointer at +0x18, its capacity at +0x30, the allocator at
// +0x38 - which is Captions::free_wstring's shape from +0x18.
void make_entry(std::uint8_t (&entry)[0x48], std::int32_t value, const char16_t* text) {
    // A message the bundle does not have (a player's own older menu.msgbnd in
    // paths.mods wins over the one made at start) gives no text, and the
    // game's wstring copy would read through null: an empty entry instead.
    if (!text) text = u"";
    std::memset(entry, 0, sizeof(entry));
    std::memcpy(&entry[0], &value, sizeof(value));
    std::memcpy(&entry[8], &text, sizeof(text));
    hle_call_guest<std::int64_t>(guest_fn(kWstrCpy), &entry[0x10], text);
    entry[0x40] = 1;
}

void free_entries(std::uint8_t* first, std::uint64_t count) {
    for (std::uint64_t k = 0; k < count; ++k) {
        Captions::free_wstring(first + k * 0x48 + 0x18);
    }
}

// Every builder takes the value its row's **Defaults** restores. The widgets
// copy it when they are built - a choice row keeps the byte at +0x368 beside
// its value pointer, a pick list the int32 at +0x3d4 - and the game's own
// Defaults row (sub_1f24850: "restore defaults?", then sub_1f1a2d0 calls every
// widget's slot +0x20) writes it back through the value pointer. So each row
// is built with what its setting ships with (host_opt_default_*), and the poll
// sees the change like any other.

// The game's slider: a byte from 0 to 10 in steps of 1 (sub_1f2ac00 builds it
// with those bounds), as Controls' Camera Sensitivity uses it.
void add_slider_row(void* dialog, std::uint8_t* value, std::uint32_t id, std::uint8_t def) {
    Captions c(id);
    hle_call_guest<std::int64_t>(guest_fn(kAddSliderRow), dialog, &c.buf[0], value, &def);
}

// An On/Off row. sub_1f2b3b0 builds the two-entry list from the game's own
// "on" and "off" messages, so the port does not have to construct a choice
// entry - the one structure here it would otherwise have to guess.
void add_toggle_row(void* dialog, void* value, std::uint32_t id, std::uint8_t def) {
    Captions c(id);
    alignas(16) std::uint8_t list[0x100]{};
    hle_call_guest<std::int64_t>(guest_fn(kBuildOnOff), &list);
    hle_call_guest<std::int64_t>(guest_fn(kAddChoiceRow), dialog, &c.buf[0], value, &list, &def);
    std::uint64_t n = 0;
    std::memcpy(&n, &list[0x98], sizeof(n));
    free_entries(list, n);
}

struct Choice {
    std::int32_t value;
    std::uint32_t id;
};

// A pick-list row. Its entries are the 0x48-byte shape above with an int32
// value, which is the same shape the game builds for Language.
void add_list_row(void* dialog, void* value, std::uint32_t id, const Choice* choices, int count, std::int32_t def) {
    Captions c(id);
    // The container is an inline array plus a count at +0x908; zeroing it is
    // the whole of its initialisation.
    alignas(16) std::uint8_t list[0x1000]{};
    for (int i = 0; i < count; ++i) {
        alignas(16) std::uint8_t entry[0x48];
        make_entry(entry, choices[i].value, message_text(choices[i].id));
        hle_call_guest<std::int64_t>(guest_fn(kListAppend), &list, &entry);
        free_entries(entry, 1);
    }
    hle_call_guest<std::int64_t>(guest_fn(kAddListRow), dialog, &c.buf[0], value, &list, &def);
    free_entries(list, static_cast<std::uint64_t>(count));
}


// The dialog's row vector, found by reading the row builder: sub_1f2a100 ends
// with sub_1f2bf60(dialog + 0xe50, &row), and that is a push_back of 0x90-byte
// records - begin at +0xe58, end at +0xe60, each holding the row's caption
// pointer at +0x00, its line help's at +0x40 and its widget at +0x80. Reading
// it back is how the port knows what the engine actually took.
constexpr std::size_t kItemsBegin = 0x0e58, kItemsEnd = 0x0e60, kItemStride = 0x90, kItemWidget = 0x80;
// The dialog's own list component, whose items are the rows (their captions
// and cursor), and within a choice widget (sub_1f2a100, 0x410 bytes) the list
// that draws its value and the pointer the value is written through.
constexpr std::size_t kDialogRows = 0x0a70, kChoiceList = 0xa0, kChoiceValue = 0x360;

int dialog_row_count(void* dialog) {
    std::uint64_t begin = 0, end = 0;
    std::memcpy(&begin, static_cast<std::uint8_t*>(dialog) + kItemsBegin, sizeof(begin));
    std::memcpy(&end, static_cast<std::uint8_t*>(dialog) + kItemsEnd, sizeof(end));
    if (!begin || end < begin || (end - begin) % kItemStride != 0) {
        return -1;
    }
    return static_cast<int>((end - begin) / kItemStride);
}

std::uint8_t* dialog_widget(void* dialog, int row) {
    std::uint64_t begin = 0, w = 0;
    std::memcpy(&begin, static_cast<std::uint8_t*>(dialog) + kItemsBegin, sizeof(begin));
    std::memcpy(&w, reinterpret_cast<std::uint8_t*>(static_cast<std::uintptr_t>(begin)) + row * kItemStride + kItemWidget,
                sizeof(w));
    return reinterpret_cast<std::uint8_t*>(static_cast<std::uintptr_t>(w));
}

void log_rows(const char* name, void* dialog, int built, int slots) {
    // Read back from the engine's own vector, so a row built and then lost is
    // loud rather than silent. sub_1f20900 appends its "Defaults" row after
    // the handler returns, so it is not counted here.
    const int took = dialog_row_count(dialog);
    host_log("pc-options: %s: %d rows built, %d in the dialog, %d slots%s", name, built, took, slots,
             took != built || took > slots ? "  <- not every row will draw" : "");
}

constexpr std::uint64_t kChoiceAppend=0x1f2c200;
void redraw(std::uint8_t* list) { hle_call_guest<std::int64_t>(guest_fn(kListRedraw),list); }
void add_text_row(void* dialog, std::uint8_t* value, std::uint32_t id, const char16_t* label, const char16_t* help,
                  const char16_t* text) {
    Captions c(id);
    std::memcpy(&c.buf[0x00], &label, sizeof(label));
    std::memcpy(&c.buf[0x40], &help, sizeof(help));
    alignas(16) std::uint8_t list[0x100]{};
    alignas(16) std::uint8_t entry[0x48];
    make_entry(entry, 0, text);
    hle_call_guest<std::int64_t>(guest_fn(kChoiceAppend), &list, &entry);
    free_entries(entry, 1);
    *value = 0;
    std::uint8_t def = 0;  // its only entry; the Key Bindings screen has no Defaults row
    hle_call_guest<std::int64_t>(guest_fn(kAddChoiceRow), dialog, &c.buf[0], value, &list, &def);
    free_entries(list, 1);
}


constexpr std::uint64_t kListVTable=0x5799fb0;

constexpr std::uint32_t kKeysPageRow=119000,kKeysFirstRow=119001;
int g_keys_page = 0;  // kept across opens, as DS3 keeps its tab
// Row 0 is the page, rows 1..7 the page's actions: the arrays below are
// indexed by row, and an action row r is action page * 7 + (r - 1).
constexpr int kKeysRows = 1 + kBindPerPage;
std::uint8_t g_keys_value[kKeysRows];
char16_t g_keys_label[kKeysRows][48];
char16_t g_keys_help[kKeysRows][160];
char16_t g_keys_text[kKeysRows][64];
struct KeysScreen {
    std::uint8_t* dialog = nullptr;
    std::uint8_t* rows = nullptr;          // the dialog's own list
    std::uint8_t* widget[kKeysRows] = {};  // each row's choice widget
    int page = -1;                         // the page the rows show
    std::uint32_t pad_was = 0;             // for Circle's and the arrows' edges
};
KeysScreen g_keys;
// A clear made on the menu thread, for the poll to save on the window thread
// with every other settings write.
std::atomic<bool> g_keys_save{false};

// ASCII into a row's UTF-16 buffer; the port's strings and SDL's key names
// are all ASCII.
template <std::size_t N>
void to_utf16(char16_t (&out)[N], const char* s) {
    std::size_t i = 0;
    for (; s && s[i] && i + 1 < N; ++i) out[i] = static_cast<char16_t>(static_cast<unsigned char>(s[i]));
    out[i] = 0;
}

// The page names, as the tabs read. Plain ASCII: the line help and values
// are drawn as HTML text, where an ampersand is not a character.
const char* const kKeyPageNames[kBindPages] = {"Movement", "Combat", "Items and Gestures", "Camera and Menus",
                                               "Other"};

// The actions listed: the Debug Menu key only when the menu is there (the
// Debug Menu plugin, engine/debug_menu.h); it is the last.
static_assert(kBindDebugMenu == kBindCount - 1, "the Debug Menu key is the last action");
int keys_listed() { return debug_menu_active() ? kBindCount : kBindDebugMenu; }

// The row's action; keys_listed() or more for a row the last, short page
// leaves blank, which shows nothing and takes no capture.
int keys_action(int row) { return g_keys.page * kBindPerPage + row - 1; }
bool keys_row_bound(int row) { return row > 0 && keys_action(row) < keys_listed(); }

// Restore Defaults: the row right after the last action, on the last page.
// The section has no room for the game's own Defaults row (its eight rows fill
// the space above the key guide), and a key binding reset is DS3's too. It
// asks twice - the first press says what the second will do - so a stray
// Enter cannot wipe a set of bindings. Moving off it, or four seconds, and
// it asks again from the start.
bool keys_row_reset(int row) { return row > 0 && keys_action(row) == keys_listed(); }
enum class KeysReset { Idle, Armed, Done };
KeysReset g_keys_reset = KeysReset::Idle;
std::chrono::steady_clock::time_point g_keys_reset_at;

void keys_fill_reset_text(int row) {
    // Short: the value column cut "Press again to restore" to "Press again to".
    to_utf16(g_keys_text[row], g_keys_reset == KeysReset::Armed  ? "Press again"
                               : g_keys_reset == KeysReset::Done ? "Restored"
                                                                 : "All actions");
}

void keys_fill_value(int row) {
    if (keys_row_reset(row)) {
        keys_fill_reset_text(row);
        return;
    }
    if (!keys_row_bound(row)) {
        g_keys_text[row][0] = 0;
        return;
    }
    char d[64];
    host_binding_describe(keys_action(row), d, sizeof(d));
    to_utf16(g_keys_text[row], d);
}

void keys_fill(int page) {
    g_keys.page = (page % kBindPages + kBindPages) % kBindPages;
    g_keys_page = g_keys.page;
    to_utf16(g_keys_label[0], "Page");
    to_utf16(g_keys_help[0], "Left and right change the group of actions shown.");
    to_utf16(g_keys_text[0], kKeyPageNames[g_keys.page]);
    for (int r = 1; r < kKeysRows; ++r) {
        if (keys_row_reset(r)) {
            to_utf16(g_keys_label[r], "Restore Defaults");
            to_utf16(g_keys_help[r], "Every action back to its default key and mouse button. Enter or click, then again "
                                     "to confirm.");
            keys_fill_reset_text(r);
            continue;
        }
        if (!keys_row_bound(r)) {
            g_keys_label[r][0] = g_keys_help[r][0] = g_keys_text[r][0] = 0;
            continue;
        }
        const BindingInfo& b = host_binding_info(keys_action(r));
        to_utf16(g_keys_label[r], b.label);
        char help[160];
        std::snprintf(help, sizeof(help), "%s Enter or click to rebind, Delete to clear.", b.help);
        to_utf16(g_keys_help[r], help);
        keys_fill_value(r);
    }
}



// Whether the remembered objects are still this screen's. The dialog is freed
// when it closes and its memory reused, so identity is checked where only the
// port could have put it: every row writes through one of its bytes.
bool screen_alive(std::uint8_t* dialog, std::uint8_t* rows, std::uint8_t* const* widget, const std::uint8_t* value,
                  int count) {
    if (!dialog || !rows) return false;
    std::uint64_t vt = 0;
    std::memcpy(&vt, rows, sizeof(vt));
    if (vt != guest(kListVTable) + 0x10 && vt != guest(kListVTable)) return false;
    for (int r = 0; r < count; ++r) {
        if (!widget[r]) return false;
        std::uint64_t v = 0;
        std::memcpy(&v, widget[r] + kChoiceValue, sizeof(v));
        if (v != reinterpret_cast<std::uint64_t>(&value[r])) return false;
    }
    return true;
}
bool keys_alive() { return screen_alive(g_keys.dialog, g_keys.rows, g_keys.widget, g_keys_value, kKeysRows); }

GUEST_ABI void pc_keys_handler(void* dialog, void* params) {
    host_log("pc-options: PCKeys opened, dialog=%p params=%p", dialog, params);
    alignas(16) std::uint8_t scratch[0x100]{};
    hle_call_guest<std::int64_t>(guest_fn(kScratchInit), &scratch);
    g_keys = KeysScreen{};
    // The Circle that opened the screen may still be down when the page row
    // first has focus; it is not a press on that row.
    g_keys.pad_was = input_delivered_buttons();
    keys_fill(g_keys_page);
    add_text_row(dialog, &g_keys_value[0], kKeysPageRow, g_keys_label[0], g_keys_help[0], g_keys_text[0]);
    for (int r = 1; r < kKeysRows; ++r) {
        add_text_row(dialog, &g_keys_value[r], kKeysFirstRow + r - 1, g_keys_label[r], g_keys_help[r], g_keys_text[r]);
    }
    log_rows("PCKeys", dialog, kKeysRows, kKeysRows);
    if (dialog_row_count(dialog) < kKeysRows) return;
    auto* d = static_cast<std::uint8_t*>(dialog);
    g_keys.rows = d + kDialogRows;
    for (int r = 0; r < kKeysRows; ++r) g_keys.widget[r] = dialog_widget(dialog, r);
    g_keys.dialog = d;
    if (!keys_alive()) {
        host_log("pc-options: PCKeys: the dialog is not laid out as expected; bindings show but cannot change here");
        g_keys = KeysScreen{};
    }
}

// The screen's per-frame work, on the menu thread. `row` is which of the
// screen's lists is updating: -2 the dialog's own, 0 the page row's value,
// 1..7 an action row's.
void keys_update(int row, bool focused) {
    bool rows_changed = false, values_changed = false;
    // Restore Defaults forgets a pending press when the cursor leaves it or
    // time runs out, and "Restored" goes back to what the row does.
    if (g_keys_reset != KeysReset::Idle &&
        ((focused && row >= 0 && !keys_row_reset(row)) ||
         std::chrono::steady_clock::now() - g_keys_reset_at > std::chrono::seconds(4))) {
        g_keys_reset = KeysReset::Idle;
        for (int r = 1; r < kKeysRows; ++r) {
            if (keys_row_reset(r)) {
                keys_fill_reset_text(r);
                redraw(g_keys.widget[r] + kChoiceList);
            }
        }
    }
    if (const int done = host_bind_capture_take_done(); done >= 0) {
        // Every row, not just the one captured: a key taken from another
        // action on this page changes that row too.
        for (int r = 1; r < kKeysRows; ++r) keys_fill_value(r);
        values_changed = true;
    }
    if (focused && row >= 0 && host_bind_capturing() < 0) {
        const std::uint32_t pad = input_delivered_buttons();
        const std::uint32_t edge = pad & ~g_keys.pad_was;
        g_keys.pad_was = pad;
        const std::uint32_t decide = menu_confirm_button();
        if (row == 0 && (edge & (decide | 0x20u | 0x80u))) {
            // Right, or Circle / a click, is the next page; Left the previous.
            keys_fill(g_keys.page + ((edge & 0x80u) ? -1 : 1));
            rows_changed = values_changed = true;
        } else if (keys_row_bound(row) && host_bind_take_clear_key()) {
            host_binding_clear(keys_action(row));
            keys_fill_value(row);
            redraw(g_keys.widget[row] + kChoiceList);
            g_keys_save.store(true, std::memory_order_relaxed);
        } else if (keys_row_bound(row) && (edge & decide)) {
            to_utf16(g_keys_text[row], "Press a key...");
            redraw(g_keys.widget[row] + kChoiceList);
            host_bind_capture_begin(keys_action(row));
        } else if (keys_row_reset(row) && (edge & decide)) {
            if (g_keys_reset == KeysReset::Armed) {
                host_bindings_load_defaults();
                g_keys_save.store(true, std::memory_order_relaxed);
                g_keys_reset = KeysReset::Done;
                for (int r = 1; r < kKeysRows; ++r) keys_fill_value(r);
                host_log("pc-options: key bindings restored to their defaults");
            } else {
                g_keys_reset = KeysReset::Armed;
            }
            g_keys_reset_at = std::chrono::steady_clock::now();
            keys_fill_reset_text(row);
            values_changed = true;
        }
    }
    if (rows_changed) redraw(g_keys.rows);
    if (values_changed) {
        for (int r = 0; r < kKeysRows; ++r) redraw(g_keys.widget[r] + kChoiceList);
    }
}

// Trophies (System > Trophies): Key Bindings' section and layout, read-only. Row 0 is
// the page, rows 1..7 seven trophies in id order. A hidden trophy keeps its name and
// description back until it is earned, as the PS4's list does. The list and what is
// earned are runtime_services.c's (runtime_trophy).
constexpr std::uint32_t kTrophyPageRow=125000,kTrophyFirstRow=125001;
constexpr int kTrophyPerPage=7,kTrophyRows=1+kTrophyPerPage;
std::uint8_t g_trophy_value[kTrophyRows];
char16_t g_trophy_label[kTrophyRows][48];
char16_t g_trophy_help[kTrophyRows][200];
char16_t g_trophy_text[kTrophyRows][32];
struct TrophyScreen {
    std::uint8_t* dialog = nullptr;
    std::uint8_t* rows = nullptr;
    std::uint8_t* widget[kTrophyRows] = {};
    int page = 0;
    std::uint32_t pad_was = 0;
};
TrophyScreen g_trophies;
int g_trophy_page = 0;  // kept across opens, as Key Bindings' page is

struct TrophyInfo {
    const char *name = nullptr, *description = nullptr;
    int grade = 0, hidden = 0;
    std::int64_t when = 0;  // 0 while locked
};
bool trophy_info(int id, TrophyInfo& t) {
    return runtime_trophy(id, &t.name, &t.description, &t.grade, &t.hidden, &t.when) != 0;
}
const char* const kTrophyGrades[] = {"", "Bronze", "Silver", "Gold", "Platinum"};

void trophies_fill(int page) {
    static constexpr int points[] = {0, 15, 30, 90, 180};  // PSN weights: progress counts points
    int count = 0, earned = 0, got = 0, all = 0;
    for (TrophyInfo t; trophy_info(count, t); ++count) {
        const int p = points[std::clamp(t.grade, 0, 4)];
        all += p;
        if (t.when) ++earned, got += p;
    }
    const int pages = std::max(1, (count + kTrophyPerPage - 1) / kTrophyPerPage);
    g_trophies.page = g_trophy_page = (page % pages + pages) % pages;
    char buf[200];
    to_utf16(g_trophy_label[0], "Page");
    std::snprintf(buf, sizeof(buf), "%d of %d trophies earned, %d%% complete. Left and right change the page.",
                  earned, count, all ? got * 100 / all : 0);
    to_utf16(g_trophy_help[0], buf);
    std::snprintf(buf, sizeof(buf), "%d / %d", g_trophies.page + 1, pages);
    to_utf16(g_trophy_text[0], buf);
    for (int r = 1; r < kTrophyRows; ++r) {
        TrophyInfo t;
        if (!trophy_info(g_trophies.page * kTrophyPerPage + r - 1, t)) {
            g_trophy_label[r][0] = g_trophy_help[r][0] = g_trophy_text[r][0] = 0;
            continue;
        }
        const char* grade = kTrophyGrades[std::clamp(t.grade, 0, 4)];
        if (t.hidden && !t.when) {
            to_utf16(g_trophy_label[r], "Hidden Trophy");
            std::snprintf(buf, sizeof(buf), "%s. Keep playing to reveal this trophy.", grade);
        } else {
            to_utf16(g_trophy_label[r], t.name);
            char date[32] = "";
            const std::time_t when = static_cast<std::time_t>(t.when);
            if (t.when > 1)
                if (const std::tm* local = std::localtime(&when)) std::strftime(date, sizeof(date), " Earned %Y-%m-%d.", local);
            std::snprintf(buf, sizeof(buf), "%s. %s%s", grade, t.description, date);
        }
        to_utf16(g_trophy_help[r], buf);
        to_utf16(g_trophy_text[r], t.when ? "Earned" : "Locked");
    }
}

bool trophies_alive() {
    return screen_alive(g_trophies.dialog, g_trophies.rows, g_trophies.widget, g_trophy_value, kTrophyRows);
}

GUEST_ABI void pc_trophies_handler(void* dialog, void* params) {
    host_log("pc-options: PCTrophies opened, dialog=%p params=%p", dialog, params);
    alignas(16) std::uint8_t scratch[0x100]{};
    hle_call_guest<std::int64_t>(guest_fn(kScratchInit), &scratch);
    g_trophies = TrophyScreen{};
    g_trophies.pad_was = input_delivered_buttons();  // the press that opened the screen
    trophies_fill(g_trophy_page);
    for (int r = 0; r < kTrophyRows; ++r)
        add_text_row(dialog, &g_trophy_value[r], r ? kTrophyFirstRow + r - 1 : kTrophyPageRow, g_trophy_label[r],
                     g_trophy_help[r], g_trophy_text[r]);
    log_rows("PCTrophies", dialog, kTrophyRows, kTrophyRows);
    if (dialog_row_count(dialog) < kTrophyRows) return;
    auto* d = static_cast<std::uint8_t*>(dialog);
    g_trophies.rows = d + kDialogRows;
    for (int r = 0; r < kTrophyRows; ++r) g_trophies.widget[r] = dialog_widget(dialog, r);
    g_trophies.dialog = d;
    if (!trophies_alive()) {
        host_log("pc-options: PCTrophies: the dialog is not laid out as expected; only the first page shows");
        g_trophies = TrophyScreen{};
    }
}

// The page row turns the page: Right or Confirm forward, Left back.
void trophies_update(int row, bool focused) {
    if (!focused || row != 0) return;
    const std::uint32_t pad = input_delivered_buttons();
    const std::uint32_t edge = pad & ~g_trophies.pad_was;
    g_trophies.pad_was = pad;
    if (!(edge & (menu_confirm_button() | 0x20u | 0x80u))) return;
    trophies_fill(g_trophies.page + ((edge & 0x80u) ? -1 : 1));
    redraw(g_trophies.rows);
    for (int r = 0; r < kTrophyRows; ++r) redraw(g_trophies.widget[r] + kChoiceList);
}

// The "Defaults" row sub_1f20900 appends after the handler returns, in a
// section that has no slot for it - Key Bindings, whose eight rows are all
// that fit above the key guide. The other three sections keep it: their
// movies carry the label slot right after the last row
// (engine/menu_assets.cpp), and their rows are built with real defaults.
// Without a slot it never drew - but the rows list still counted it, and
// pressing Down past the last row scrolled the captions up by one while the
// values, which are separate lists, stayed where they were: every caption
// beside the wrong value. So there it comes off again, the way a vector
// element is destroyed -
// its two caption copies freed, its widget unreferenced - and the rows are
// laid out again from the vector, as sub_1f20900 itself last did.
void drop_defaults_row(std::uint8_t* dialog) {
    std::uint64_t begin = 0, end = 0;
    std::memcpy(&begin, dialog + kItemsBegin, sizeof(begin));
    std::memcpy(&end, dialog + kItemsEnd, sizeof(end));
    if (!begin || end < begin + kItemStride) return;
    auto* rec = reinterpret_cast<std::uint8_t*>(static_cast<std::uintptr_t>(end - kItemStride));
    const char16_t* label = nullptr;
    std::memcpy(&label, rec, sizeof(label));
    if (label != message_text(kDefaultsRowId)) {
        host_log("pc-options: the last row is not Defaults; left alone");
        return;
    }
    Captions::free_wstring(rec + 0x08);
    Captions::free_wstring(rec + 0x48);
    std::uint64_t widget = 0;
    std::memcpy(&widget, rec + kItemWidget, sizeof(widget));
    if (widget) {
        // DLReferenceCountObject: the count at +8, and the last reference
        // calls the deleting destructor, slot 0.
        auto* w = reinterpret_cast<std::uint8_t*>(static_cast<std::uintptr_t>(widget));
        std::int32_t refs = 0;
        std::memcpy(&refs, w + 8, sizeof(refs));
        const std::int32_t left = refs - 1;
        std::memcpy(w + 8, &left, sizeof(left));
        if (refs == 1) {
            std::uint64_t vt = 0, dtor = 0;
            std::memcpy(&vt, w, sizeof(vt));
            std::memcpy(&dtor, reinterpret_cast<void*>(static_cast<std::uintptr_t>(vt)), sizeof(dtor));
            hle_call_guest<std::int64_t>(reinterpret_cast<void*>(static_cast<std::uintptr_t>(dtor)), w);
        }
    }
    end -= kItemStride;
    std::memcpy(dialog + kItemsEnd, &end, sizeof(end));
    hle_call_guest<std::int64_t>(guest_fn(kLayoutRows), dialog, 0);
}


struct DefaultsView {
    std::uint8_t* dialog=nullptr;
    std::uint8_t* pick=nullptr;
    bool hidden=false;
    char16_t first=0;
};
DefaultsView g_defaults_view;
char16_t g_defaults_caption[32];
constexpr std::size_t kPickItems=0x100;
void keep_defaults_row(std::uint8_t* dialog, int pick_row) {
    g_defaults_view = DefaultsView{};
    const int rows = dialog_row_count(dialog);
    if (rows < 1) return;
    std::uint64_t begin = 0;
    std::memcpy(&begin, dialog + kItemsBegin, sizeof(begin));
    auto* rec = reinterpret_cast<std::uint8_t*>(static_cast<std::uintptr_t>(begin)) + (rows - 1) * kItemStride;
    const char16_t* label = nullptr;
    std::memcpy(&label, rec, sizeof(label));
    if (!label || label != message_text(kDefaultsRowId)) {
        host_log("pc-options: the last row is not Defaults; left alone");
        return;
    }
    std::size_t n = 0;
    while (label[n] && n + 1 < sizeof(g_defaults_caption) / sizeof(g_defaults_caption[0])) {
        g_defaults_caption[n] = label[n];
        ++n;
    }
    g_defaults_caption[n] = 0;
    const char16_t* ours = g_defaults_caption;
    std::memcpy(rec, &ours, sizeof(ours));
    hle_call_guest<std::int64_t>(guest_fn(kLayoutRows), dialog, 0);
    g_defaults_view.dialog = dialog;
    g_defaults_view.pick = pick_row >= 0 && pick_row < rows - 1 ? dialog_widget(dialog, pick_row) : nullptr;
}

// From the list update: the pick list took focus, or the rows got it back.
void defaults_view_update(std::uint8_t* comp, bool focused) {
    DefaultsView& v = g_defaults_view;
    if (!v.dialog || !v.pick || !focused) return;
    if (comp == v.pick + kPickItems && !v.hidden) {
        v.hidden = true;
        v.first = g_defaults_caption[0];
        g_defaults_caption[0] = 0;
        redraw(v.dialog + kDialogRows);
    } else if (comp == v.dialog + kDialogRows && v.hidden) {
        v.hidden = false;
        g_defaults_caption[0] = v.first;
        redraw(v.dialog + kDialogRows);
    }
}

// Opening a section: name it, hand sub_1f20900 the handler. The name goes
// through g_open_name, the table slot the port owns. `pick_row` is the row
// of the section's pick list, or -1; -2 means the section has no Defaults
// row (no slot for it in its movie), so the engine's is taken off again.
std::int64_t open_named(void* root, void* params, const char* name, void* handler, int pick_row) {
    std::snprintf(g_open_name, sizeof(g_open_name), "%s", name);
    g_defaults_view = DefaultsView{};
    const std::int64_t dialog =
        hle_call_guest<std::int64_t>(guest_fn(kOpenSection), root, params, g_open_name, handler, 0, 0);
    auto* d = reinterpret_cast<std::uint8_t*>(static_cast<std::uintptr_t>(dialog));
    if(d && pick_row==-2) drop_defaults_row(d);
    else if (d) {
        keep_defaults_row(d, pick_row);
    }
    return dialog;
}


// Values are owned by this page, and written to the existing atomics only after
// the engine's slider changes. Opening the page never overwrites another frontend.
struct Slider {
    std::atomic<float>* setting;
    std::uint32_t id;
    float base,step;
    std::uint8_t def,value=0,last=0;
};
Slider camera[] = {
    {nullptr,118004,1.0f,0.05f,0},
    {nullptr,121000,0.5f,0.10f,5},
    {nullptr,121001,0.5f,0.10f,5}
};
bool seeded=false;
bool camera_page=false,graphics_page=false,input_page=false;
Slider graphics_sliders[]={{nullptr,117007,0,0.2f,5},{nullptr,117006,1,0.2f,0}};
Slider effect_sliders[]={{nullptr,120000,0,0.1f,10},{nullptr,120002,0,0.2f,5}};
struct StartupToggle { std::atomic<bool>* setting=nullptr; std::uint32_t id; std::uint8_t value=0,last=0; };
StartupToggle enhancements[]={{nullptr,124001},{nullptr,124002},{nullptr,124004}};
bool enhancements_seeded=false;
StartupToggle graphics_toggles[]={{nullptr,117001},{nullptr,117000}};
StartupToggle effect_toggles[]={{nullptr,117005},{nullptr,117002},{nullptr,117003},{nullptr,120001}};
bool graphics_seeded=false,effects_seeded=false;
std::int32_t output_resolution=1,last_resolution=1;
const Choice outputs[]={{0,117010},{1,117012},{2,117013},{3,117015},{4,117040},{5,117041},{6,117042},{7,117043},{8,117044},{9,117045}};
template<typename Rows> void SeedSliders(Rows& rows) {
    for (auto& s:rows) s.value=s.last=static_cast<std::uint8_t>(std::clamp(std::lround((s.setting->load()-s.base)/s.step),0l,10l));
}
void SeedCamera() {
    for (auto& s:camera) s.value=s.last=static_cast<std::uint8_t>(std::clamp(std::lround((s.setting->load()-s.base)/s.step),0l,10l));
    seeded=true;
}
GUEST_ABI void CameraHandler(void* dialog,void* params) {
    host_log("pc-options: PCCamera opened, dialog=%p params=%p",dialog,params);
    alignas(16) std::uint8_t scratch[0x100]{};
    hle_call_guest<std::int64_t>(guest_fn(kScratchInit),&scratch);
    SeedCamera();
    for (auto& s:camera) add_slider_row(dialog,&s.value,s.id,s.def);
    log_rows("PCCamera",dialog,3,3);
}
GUEST_ABI std::int64_t OpenCamera(void* root,void* params) {
    return open_named(root,params,"PCCamera",reinterpret_cast<void*>(&CameraHandler),-1);
}
GUEST_ABI void* OpenCameraRow(void* out,void* ctx) {
    GuestFunction f(kOpenerFunctorVTable,reinterpret_cast<void*>(&OpenCamera));
    hle_call_guest<std::int64_t>(guest_fn(kOpenSectionFlow),out,ctx,f.buf);
    return out;
}
GUEST_ABI void EnhancementsHandler(void* dialog,void* params) {
    host_log("pc-options: PCEnhance opened, dialog=%p params=%p",dialog,params);
    alignas(16) std::uint8_t scratch[0x100]{};
    hle_call_guest<std::int64_t>(guest_fn(kScratchInit),&scratch);
    for (auto& t:enhancements) { t.value=t.last=t.setting->load()?1:0;add_toggle_row(dialog,&t.value,t.id,0); }
    enhancements_seeded=true;
    log_rows("PCEnhance",dialog,3,3);
}
GUEST_ABI std::int64_t OpenEnhancements(void* root,void* params) {
    return open_named(root,params,"PCEnhance",reinterpret_cast<void*>(&EnhancementsHandler),-1);
}
GUEST_ABI void* OpenEnhancementsRow(void* out,void* ctx) {
    GuestFunction f(kOpenerFunctorVTable,reinterpret_cast<void*>(&OpenEnhancements));
    hle_call_guest<std::int64_t>(guest_fn(kOpenSectionFlow),out,ctx,f.buf);return out;
}
GUEST_ABI void GraphicsHandler(void* dialog,void* params) {
    host_log("pc-options: PCGraphics opened, dialog=%p params=%p",dialog,params);
    alignas(16) std::uint8_t scratch[0x100]{};
    hle_call_guest<std::int64_t>(guest_fn(kScratchInit),&scratch);
    for (auto& t:graphics_toggles) { t.value=t.last=t.setting->load()?1:0;add_toggle_row(dialog,&t.value,t.id,1); }
    SeedSliders(graphics_sliders);
    for (auto& s:graphics_sliders) add_slider_row(dialog,&s.value,s.id,s.def);
    output_resolution=last_resolution=BbSettings::Get().output_res;
    add_list_row(dialog,&output_resolution,117004,outputs,BbSettings::OutputCount,BbSettings::OutputDefault);
    graphics_seeded=true;log_rows("PCGraphics",dialog,5,5);
}
GUEST_ABI void EffectsHandler(void* dialog,void* params) {
    host_log("pc-options: PCEffects opened, dialog=%p params=%p",dialog,params);
    alignas(16) std::uint8_t scratch[0x100]{};
    hle_call_guest<std::int64_t>(guest_fn(kScratchInit),&scratch);
    for (auto& t:effect_toggles) { t.value=t.last=t.setting->load()?1:0;add_toggle_row(dialog,&t.value,t.id,1); }
    SeedSliders(effect_sliders);
    for (auto& s:effect_sliders) add_slider_row(dialog,&s.value,s.id,s.def);
    effects_seeded=true;log_rows("PCEffects",dialog,6,6);
}
GUEST_ABI std::int64_t OpenGraphics(void* root,void* params) {
    return open_named(root,params,"PCGraphics",reinterpret_cast<void*>(&GraphicsHandler),4);
}
GUEST_ABI std::int64_t OpenEffects(void* root,void* params) {
    return open_named(root,params,"PCEffects",reinterpret_cast<void*>(&EffectsHandler),-1);
}
GUEST_ABI void* OpenGraphicsRow(void* out,void* ctx) {
    GuestFunction f(kOpenerFunctorVTable,reinterpret_cast<void*>(&OpenGraphics));
    hle_call_guest<std::int64_t>(guest_fn(kOpenSectionFlow),out,ctx,f.buf);return out;
}
GUEST_ABI void* OpenEffectsRow(void* out,void* ctx) {
    GuestFunction f(kOpenerFunctorVTable,reinterpret_cast<void*>(&OpenEffects));
    hle_call_guest<std::int64_t>(guest_fn(kOpenSectionFlow),out,ctx,f.buf);return out;
}

StartupToggle controls[]={{nullptr,118000},{nullptr,118002},{nullptr,118003},{nullptr,116002},{nullptr,118005}};
std::uint8_t sensitivity=5,last_sensitivity=5;bool controls_seeded=false;
GUEST_ABI void ControlsHandler(void* dialog,void*) {
    alignas(16) std::uint8_t scratch[0x100]{};hle_call_guest<std::int64_t>(guest_fn(kScratchInit),&scratch);
    auto& s=BbSettings::Get();sensitivity=last_sensitivity=s.mouse_sensitivity;
    for(auto& t:controls){t.value=t.last=t.setting->load()?1:0;}
    add_toggle_row(dialog,&controls[0].value,118000,1);
    add_slider_row(dialog,&sensitivity,118001,5);
    for(int i=1;i<5;i++)add_toggle_row(dialog,&controls[i].value,controls[i].id,i==3?1:0);
    controls_seeded=true;log_rows("PCControls",dialog,6,6);
}
GUEST_ABI std::int64_t OpenControls(void* root,void* params) {return open_named(root,params,"PCControls",reinterpret_cast<void*>(&ControlsHandler),-1);}
GUEST_ABI std::int64_t OpenKeys(void* root,void* params) {return open_named(root,params,"PCKeys",reinterpret_cast<void*>(&pc_keys_handler),-2);}
GUEST_ABI void* OpenControlsRow(void* out,void* ctx) {GuestFunction f(kOpenerFunctorVTable,reinterpret_cast<void*>(&OpenControls));hle_call_guest<std::int64_t>(guest_fn(kOpenSectionFlow),out,ctx,f.buf);return out;}
GUEST_ABI void* OpenKeysRow(void* out,void* ctx) {GuestFunction f(kOpenerFunctorVTable,reinterpret_cast<void*>(&OpenKeys));hle_call_guest<std::int64_t>(guest_fn(kOpenSectionFlow),out,ctx,f.buf);return out;}
GUEST_ABI std::int64_t OpenTrophies(void* root,void* params) {return open_named(root,params,"PCTrophies",reinterpret_cast<void*>(&pc_trophies_handler),-2);}
GUEST_ABI void* OpenTrophiesRow(void* out,void* ctx) {GuestFunction f(kOpenerFunctorVTable,reinterpret_cast<void*>(&OpenTrophies));hle_call_guest<std::int64_t>(guest_fn(kOpenSectionFlow),out,ctx,f.buf);return out;}
GUEST_ABI void* SystemFinalize(void* out,void* builder,void* arg3) {
    std::int64_t flag=0;
    { Captions c(110013);GuestFunction f(kRowFunctorVTable,reinterpret_cast<void*>(&OpenEnhancementsRow));
      hle_call_guest<std::int64_t>(guest_fn(kAddCommandRow),builder,c.buf,f.buf,&flag); }
    if (graphics_page) {
        { Captions c(110007);GuestFunction f(kRowFunctorVTable,reinterpret_cast<void*>(&OpenGraphicsRow));
          hle_call_guest<std::int64_t>(guest_fn(kAddCommandRow),builder,c.buf,f.buf,&flag); }
        { Captions c(110010);GuestFunction f(kRowFunctorVTable,reinterpret_cast<void*>(&OpenEffectsRow));
          hle_call_guest<std::int64_t>(guest_fn(kAddCommandRow),builder,c.buf,f.buf,&flag); }
    }
    if(input_page) {
        {Captions c(110008);GuestFunction f(kRowFunctorVTable,reinterpret_cast<void*>(&OpenControlsRow));hle_call_guest<std::int64_t>(guest_fn(kAddCommandRow),builder,c.buf,f.buf,&flag);}
        {Captions c(110009);GuestFunction f(kRowFunctorVTable,reinterpret_cast<void*>(&OpenKeysRow));hle_call_guest<std::int64_t>(guest_fn(kAddCommandRow),builder,c.buf,f.buf,&flag);}
    }
    if (camera_page) { Captions c(110011);GuestFunction f(kRowFunctorVTable,reinterpret_cast<void*>(&OpenCameraRow));
      hle_call_guest<std::int64_t>(guest_fn(kAddCommandRow),builder,c.buf,f.buf,&flag); }
    { Captions c(110014);GuestFunction f(kRowFunctorVTable,reinterpret_cast<void*>(&OpenTrophiesRow));
      hle_call_guest<std::int64_t>(guest_fn(kAddCommandRow),builder,c.buf,f.buf,&flag); }
    hle_call_guest<std::int64_t>(guest_fn(kSystemFinalize),out,builder,arg3);
    host_log("pc-options: System list includes PC Enhancements%s%s",graphics_page?", PC Graphics / PC Effects":"",camera_page?", PC Camera":"");
    return out;
}

// Quit Game on the title menu, adapted from bbhost engine/option_menu.cpp. The title's
// command list (sub_1f4a3d0: Continue, Load Game, New Game, System, Log In) is finalised
// by one call, sub_1fea820 at 0x1f4adb3; TitleFinalize adds a "Quit Game" row first
// (sub_1f4c9a0, the way System's Exit Game is added). Its opener is built like the
// game's own Exit Game opener (sub_200c9f0): a message box from a group-204 caption
// (sub_201ed10), a YES command around an action (sub_1d5db60) and the popup of the two
// (sub_201a070), so the question is the menu's own, modal, with YES and NO. YES pushes
// SDL_EVENT_QUIT: the same shutdown as closing the window. The movie's sixth line is
// menu_assets' title mode.
constexpr std::uint64_t kTitleFinalize=0x1fea820,kAddExitRow=0x1f4c9a0;
constexpr std::uint64_t kMessageBox=0x201ed10,kYesCommand=0x1d5db60,kPopup=0x201a070;
constexpr std::uint32_t kQuitRowId=122000,kQuitQuestion=920001,kMsgDialogText=0xcc;
GUEST_ABI std::int64_t QuitYes(std::int64_t,std::int64_t) {
    host_log("pc-options: Quit Game");
    SDL_Event quit{}; quit.type=SDL_EVENT_QUIT; SDL_PushEvent(&quit);
    return 0;
}
void release_engine_ref(std::uint64_t obj) {
    if (!obj) return;
    auto* rc=reinterpret_cast<std::int32_t*>(obj+8);
    if ((*rc)--!=1) return;
    std::uint64_t vt=0,dtor=0;
    std::memcpy(&vt,reinterpret_cast<void*>(obj),8); std::memcpy(&dtor,reinterpret_cast<void*>(vt),8);
    Call(dtor,obj);
}
GUEST_ABI void* QuitOpener(void* out,void* factory) {
    alignas(16) std::uint8_t caption[0x40]{};
    hle_call_guest<std::int64_t>(guest_fn(kMsgRepository),&caption[0],kMsgDialogText,kQuitQuestion);
    std::uint64_t box=0,command=0;
    hle_call_guest<std::int64_t>(guest_fn(kMessageBox),&box,factory,&caption[0]);
    GuestFunction action(kOpenerFunctorVTable,reinterpret_cast<void*>(&QuitYes));
    hle_call_guest<std::int64_t>(guest_fn(kYesCommand),&command,action.buf,2);
    hle_call_guest<std::int64_t>(guest_fn(kPopup),out,&box,&command);
    release_engine_ref(command); release_engine_ref(box);
    Captions::free_wstring(&caption[0x08]);
    return out;
}
GUEST_ABI std::int64_t TitleFinalize(void* out,void* builder,void* arg3) {
    { Captions c(kQuitRowId);GuestFunction f(kRowFunctorVTable,reinterpret_cast<void*>(&QuitOpener));
      hle_call_guest<std::int64_t>(guest_fn(kAddExitRow),builder,c.buf,f.buf); }
    return hle_call_guest<std::int64_t>(guest_fn(kTitleFinalize),out,builder,arg3);
}
} // namespace

bool Install(std::uint8_t* image,std::size_t size) {
    if (!BbSettings::Get().camera_controls && !BbSettings::Get().change_appearance && !BbSettings::Get().rebirth && !BbSettings::Get().graphics_controls && !BbSettings::Get().pc_controls) return false;
    const char* assets=std::getenv("BB_PC_MENU_ASSETS");
    const char* game=std::getenv("BB_GAME_DIR");
    if (!assets || !*assets || !game || !*game) return false;
    // Validate all four patch regions together while the loader owns the RW image.
    constexpr std::size_t table=0x53398b0+6*8,imm=0x1b20e21,call=0x1bb4978,pad=0x1bb4c60;
    constexpr std::uint8_t expected[]={0x90,0x66,0x66,0x66,0x66,0x66,0x66,0x2e,0x0f,0x1f,0x84,0,0,0,0,0};
    if (size<table+16 || size<imm+1 || size<call+5 || size<pad+16) return false;
    std::uint64_t zeros[2]{}; std::int32_t rel=0;
    std::memcpy(&rel,image+call+1,4);
    if (std::memcmp(image+table,zeros,16) || image[imm]!=6 || image[call]!=0xe8 ||
        static_cast<std::int64_t>(call+5)+rel!=0x1bea630 || std::memcmp(image+pad,expected,16) ||
        !MenuMemory::Compatible(image,size)) {
        host_log("pc-options disabled: System bytes differ from verified 1.09"); return false;
    }
    namespace fs=std::filesystem;
    std::vector<std::pair<std::string,std::string>> mounts;
    bool title_quit=false;
    try {
        const fs::path root=fs::u8path(assets),original=fs::u8path(game);
        auto add=[&](const fs::path& relative) {
            const auto prepared=root/relative;
            const auto path="/app0/"+relative.generic_string();
            char resolved[512]{}; std::error_code ec;
            if (!fs::is_regular_file(prepared) || runtime_file_translate(path.c_str(),resolved,sizeof resolved) ||
                !fs::equivalent(fs::u8path(resolved),original/relative,ec) || ec)
                return false; // A player's own file wins; never bind a foreign movie.
            mounts.emplace_back(path,prepared.string()); return true;
        };
        if (!add("dvdroot_ps4/menu/optionsetting.gfx")) return false;
        for (const auto& item:fs::directory_iterator(original/"dvdroot_ps4/msg")) {
            if (!item.is_directory()) continue;
            const auto relative=fs::path("dvdroot_ps4/msg")/item.path().filename()/"menu.msgbnd.dcx";
            if (fs::is_regular_file(original/relative) && !add(relative)) return false;
        }
        if (mounts.size()<2 || mounts.size()>24) return false;
        // Quit Game needs every title movie the dump has prepared with its sixth line.
        title_quit=true;
        for (const char* movie:{"dvdroot_ps4/menu/title.gfx","dvdroot_ps4/menu/title_dlc.gfx","dvdroot_ps4/menu/title_dlc_eu.gfx"})
            if (fs::is_regular_file(original/movie)) title_quit=add(movie) && title_quit;
    } catch (const std::exception& e) { host_log("pc-options disabled: %s",e.what()); return false; }
    unsigned installed=0;
    for (const auto& [path,file]:mounts) {
        if (runtime_file_mount(path.c_str(),file.c_str())) {
            for (unsigned i=0;i<installed;++i) runtime_file_unmount(mounts[i].first.c_str());
            return false;
        }
        ++installed;
    }
    g_slide=reinterpret_cast<std::uint64_t>(image);
    MenuMemory::Install(image,size);
    auto& settings=BbSettings::Get();
    input_page=settings.pc_controls;
    controls[0].setting=&settings.mouse_camera;controls[1].setting=&settings.mouse_invert_x;
    controls[2].setting=&settings.mouse_invert_y;controls[3].setting=&settings.mouse_menu;
    controls[4].setting=&settings.mouse_auto_rotation;
    camera_page=settings.camera_controls;
    graphics_page=Graphics::Enabled();
    graphics_toggles[0].setting=&settings.effects[4];graphics_toggles[1].setting=&settings.effects[3];
    graphics_sliders[0].setting=&settings.graphics_ao_strength;graphics_sliders[1].setting=&settings.graphics_shadow_scale;
    effect_toggles[0].setting=&settings.effects[2];effect_toggles[1].setting=&settings.effects[1];
    effect_toggles[2].setting=&settings.effects[0];effect_toggles[3].setting=&settings.graphics_vignette;
    effect_sliders[0].setting=&settings.graphics_bloom;effect_sliders[1].setting=&settings.graphics_saturation;
    enhancements[0].setting=&settings.change_appearance;
    enhancements[1].setting=&settings.rebirth;
    enhancements[2].setting=&settings.camera_controls;
    camera[0].setting=&settings.camera_fov_scale;
    camera[1].setting=&settings.camera_distance_scale;
    camera[2].setting=&settings.camera_height_scale;
    const auto name=reinterpret_cast<std::uint64_t>(g_open_name);
    std::memcpy(image+table,&name,8); image[imm]=7;
    std::uint8_t* p=image+pad; *p++=0x49; *p++=0xbb;
    const auto target=reinterpret_cast<std::uint64_t>(&SystemFinalize);
    std::memcpy(p,&target,8); p+=8; *p++=0x41; *p++=0xff; *p=0xe3;
    const std::int32_t to_pad=pad-(call+5); std::memcpy(image+call+1,&to_pad,4);
    // Title Quit Game: the same redirect for the title list's finalise call, through
    // alignment padding after a non-returning call (verified 1.09 bytes).
    constexpr std::size_t tcall=0x1b4adb3,tpad=0x1b25c20;
    std::int32_t trel=0;
    if (title_quit && size>=tcall+5) std::memcpy(&trel,image+tcall+1,4);
    if (title_quit && size>=tpad+16 && image[tcall]==0xe8 && static_cast<std::int64_t>(tcall+5)+trel==0x1bea820 &&
        !std::memcmp(image+tpad,expected,16)) {
        std::uint8_t* q=image+tpad; *q++=0x49; *q++=0xbb;
        const auto title=reinterpret_cast<std::uint64_t>(&TitleFinalize);
        std::memcpy(q,&title,8); q+=8; *q++=0x41; *q++=0xff; *q=0xe3;
        const std::int32_t to_tpad=tpad-(tcall+5); std::memcpy(image+tcall+1,&to_tpad,4);
        host_log("pc-options: title menu Quit Game installed");
    } else {
        host_log("pc-options: title menu Quit Game not installed (%s)",title_quit?"bytes differ from verified 1.09":"title movies not prepared");
    }
    g_installed=true;
    host_log("pc-options: native System pages installed (%u local assets): Enhancements%s%s",installed,graphics_page?", Graphics / Effects":"",camera_page?", Camera":"");
    return true;
}
bool ClickDecides(std::uint8_t* list, int index) {
    return g_installed && g_keys.dialog && list == g_keys.rows && index >= 1 && index < kKeysRows &&
           (keys_row_bound(index) || keys_row_reset(index)) && keys_alive();
}

void ListUpdate(std::uint8_t* comp, bool focused) {
    if (!g_installed) {
        return;
    }
    auto* c = static_cast<std::uint8_t*>(comp);
    defaults_view_update(c, focused);
    if (g_trophies.dialog && (c == g_trophies.rows || c == g_trophies.widget[0] + kChoiceList)) {
        if (!trophies_alive()) {
            g_trophies = TrophyScreen{};
            return;
        }
        trophies_update(c == g_trophies.rows ? -2 : 0, focused);
        return;
    }
    if (!g_keys.dialog) {
        return;
    }
    int row = -1;
    if (c == g_keys.rows) {
        row = -2;
    } else {
        for (int r = 0; r < kKeysRows && row == -1; ++r) {
            if (c == g_keys.widget[r] + kChoiceList) row = r;
        }
    }
    if (row == -1) {
        return;
    }
    if (!keys_alive()) {
        g_keys = KeysScreen{};
        return;
    }
    keys_update(row, focused);
}


void Tick() {
    if (!g_installed) return;
    bool changed=false;
    for (auto& s:camera) {
        if (!seeded) break;
        if (s.value==s.last) continue;
        s.last=std::min<std::uint8_t>(s.value,10);
        *s.setting=s.base+s.last*s.step;
        changed=true;
    }
    if (enhancements_seeded) for (auto& t:enhancements) {
        if (t.value==t.last) continue;
        t.last=t.value;
        *t.setting=t.value!=0;
        changed=true;
    }
    auto push_sliders=[&](auto& rows,bool active) {
        if (!active) return;
        for (auto& s:rows) if (s.value!=s.last) { s.last=std::min<std::uint8_t>(s.value,10);*s.setting=s.base+s.last*s.step;changed=true; }
    };
    auto push_toggles=[&](auto& rows,bool active) {
        if (!active) return;
        for (auto& t:rows) if (t.value!=t.last) { t.last=t.value;*t.setting=t.value!=0;changed=true; }
    };
    push_sliders(graphics_sliders,graphics_seeded);push_sliders(effect_sliders,effects_seeded);
    push_toggles(controls,controls_seeded);
    if(controls_seeded && sensitivity!=last_sensitivity){last_sensitivity=std::min<std::uint8_t>(sensitivity,10);BbSettings::Get().mouse_sensitivity=last_sensitivity;changed=true;}
    push_toggles(graphics_toggles,graphics_seeded);push_toggles(effect_toggles,effects_seeded);
    if (graphics_seeded && output_resolution!=last_resolution) {
        last_resolution=std::clamp(output_resolution,0,BbSettings::OutputCount-1);
        BbSettings::Get().output_res=last_resolution;changed=true;
    }
    if (changed) { BbSettings::Save(); host_log("pc-options: System option changed; saved to bbport.ini"); }
}
} // namespace BbEngine::Options
