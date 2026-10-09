// SPDX-License-Identifier: GPL-3.0-or-later
// Native System page and guest row builders adapted from bbhost option_menu.cpp.
// The existing renderer and bbport.ini remain authoritative.
#include "option_menu.h"
#include "engine_hooks.h"
#include "menu_memory.h"
#include "bbport_platform.h"
#include "bbport_settings.h"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>
extern "C" int runtime_file_translate(const char*,char*,std::size_t);
extern "C" int runtime_file_mount(const char*,const char*);
extern "C" void runtime_file_unmount(const char*);
namespace BbEngine::Options {
namespace {
#define GUEST_ABI __attribute__((sysv_abi))
std::uint64_t g_slide=0;
bool g_installed=false;
constexpr std::uint64_t kPreferredGuestSlide=0x400000;
constexpr std::uint64_t kOpenSection=0x1f20900,kScratchInit=0x208af20,kAddSliderRow=0x1f2ac00;
constexpr std::uint64_t kAddChoiceRow=0x1f2a100,kBuildOnOff=0x1f2b3b0,kWstrCpy=0x2d67040;
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

struct DefaultsView {
    std::uint8_t* dialog=nullptr;
    std::uint8_t* pick=nullptr;
    bool hidden=false;
    char16_t first=0;
};
DefaultsView g_defaults_view;
char16_t g_defaults_caption[32];
constexpr std::size_t kPickItems=0x100;
void redraw(std::uint8_t* list) { hle_call_guest<std::int64_t>(guest_fn(kListRedraw),list); }
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
    if (d) {
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
GUEST_ABI void* SystemFinalize(void* out,void* builder,void* arg3) {
    Captions c(110011);
    GuestFunction f(kRowFunctorVTable,reinterpret_cast<void*>(&OpenCameraRow));
    std::int64_t flag=0;
    hle_call_guest<std::int64_t>(guest_fn(kAddCommandRow),builder,c.buf,f.buf,&flag);
    hle_call_guest<std::int64_t>(guest_fn(kSystemFinalize),out,builder,arg3);
    host_log("pc-options: System list includes PC Camera");
    return out;
}
} // namespace

bool Install(std::uint8_t* image,std::size_t size) {
    if (!BbSettings::Get().camera_controls) return false;
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
    camera[0].setting=&settings.camera_fov_scale;
    camera[1].setting=&settings.camera_distance_scale;
    camera[2].setting=&settings.camera_height_scale;
    const auto name=reinterpret_cast<std::uint64_t>(g_open_name);
    std::memcpy(image+table,&name,8); image[imm]=7;
    std::uint8_t* p=image+pad; *p++=0x49; *p++=0xbb;
    const auto target=reinterpret_cast<std::uint64_t>(&SystemFinalize);
    std::memcpy(p,&target,8); p+=8; *p++=0x41; *p++=0xff; *p=0xe3;
    const std::int32_t to_pad=pad-(call+5); std::memcpy(image+call+1,&to_pad,4);
    g_installed=true;
    host_log("pc-options: native System -> PC Camera installed (%u local assets)",installed);
    return true;
}
void Tick() {
    if (!g_installed || !seeded) return;
    bool changed=false;
    for (auto& s:camera) {
        if (s.value==s.last) continue;
        s.last=std::min<std::uint8_t>(s.value,10);
        *s.setting=s.base+s.last*s.step;
        changed=true;
    }
    if (changed) { BbSettings::Save(); host_log("pc-options: camera changed in System; saved to bbport.ini"); }
}
} // namespace BbEngine::Options
