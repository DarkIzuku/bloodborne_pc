// SPDX-License-Identifier: GPL-3.0-or-later
#include "button_prompts.h"

#include "bc7.h"
#include "../bbport_overlay.h"
#include "../input/bindings.h"
#include "../input/input.h"
#include "core/memory.h"

#include <SDL3/SDL.h>
#include <xxhash.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "imstb_truetype.h"
#pragma GCC diagnostic pop

// Xbox labels are drawn here; no third-party or game textures are bundled.
namespace BbPrompts {
namespace {

enum Mode { kUnset = -1, kPlayStation, kXbox, kKeyboard };
const char* const kModeNames[] = {"PlayStation", "Xbox", "keyboard and mouse"};

// CUSA03173 1.09's menu/common.tpf.dcx, by XXH3 (tools/engine/button_prompts.py prints them).
constexpr int kGlyphs = 20, kSpots = 3;
constexpr std::uint64_t kGlyphSize = 1024, kAtlasSize = 1024 * 512;
constexpr std::uint64_t kGlyphHash[kGlyphs] = {
    0x681cd54bf1ebfaeaull, // KG_Cancel
    0xc2e52c29caf88e58ull, // KG_L1
    0xa865b38e932d16bbull, // KG_L2
    0x22fb8049bd638e46ull, // KG_L3
    0x67620ed3bf36878aull, // KG_LS
    0x29ad9450b1d02186ull, // KG_L_D
    0x8e97d270d7b066cfull, // KG_L_LR
    0xe11b417f55904354ull, // KG_L_U
    0xde3b2100c5c414b1ull, // KG_L_UD
    0x3dd2dbcbb8432b36ull, // KG_L_UDLR
    0xc42aa25bf0baffedull, // KG_OK
    0xb3e95215d4a042adull, // KG_R1
    0xc80ea56ca1f84870ull, // KG_R2
    0x568eba23a284c2a8ull, // KG_R3
    0x0c1d8189222d57a6ull, // KG_RS
    0xd8b19c9041c32644ull, // KG_R_L
    0x18ba522de454258dull, // KG_R_U
    0xbbf2bc267e780389ull, // KG_Start
    0xe6e94727c7bcf1bbull, // KG_TP_L
    0xdae39773f2aadd03ull, // KG_TP_R
};
constexpr std::uint64_t kAtlasHash = 0x2706953d47319687ull; // MENU_Common_00091, 1024x512
// The HUD's Circle, Cross and L3 in that atlas, on whole 4x4 blocks with nothing else in them.
constexpr int kSpotX[kSpots] = {660, 692, 724}, kSpotY = 120, kSpotW = 28, kSpotH = 32;
constexpr std::size_t kSpotBytes = kSpotW * kSpotH * 4;

// What a keycap shows for each: an action's key or mouse button; two keys (a d-pad axis); the
// four of a stick or d-pad as the keys sit (up above left, down, right); the mouse (the camera).
enum Kind { kAction, kPair, kCluster, kCamera };
struct Face {
    Kind kind;
    int a = 0, b = 0, c = 0, d = 0;
};
// The key guide names menu buttons (OK, Cancel) and the pad's for gameplay
// ("<KG_R_U>: Restore HP"): the PC actions on those bits (input/bindings.cpp).
constexpr Face kGlyphFaces[kGlyphs] = {
    {kAction, kBindBack},
    {kAction, kBindTransform},
    {kAction, kBindFirearm},
    {kAction, kBindL3},
    {kCluster, kBindMoveF, kBindMoveL, kBindMoveB, kBindMoveR},
    {kAction, kBindSwitchItem},
    {kPair, kBindSwitchLeft, kBindSwitchRight},
    {kAction, kBindDpadUp},
    {kPair, kBindDpadUp, kBindSwitchItem},
    {kCluster, kBindDpadUp, kBindSwitchLeft, kBindSwitchItem, kBindSwitchRight},
    {kAction, kBindConfirm},
    {kAction, kBindAttack},
    {kAction, kBindStrong},
    {kAction, kBindLockOn},
    {kCamera},
    {kAction, kBindUseItem},
    {kAction, kBindBloodVial},
    {kAction, kBindMenu},
    {kAction, kBindGestures},
    {kAction, kBindEffects},
};
constexpr Face kSpotFaces[kSpots] = {{kAction, kBindRoll}, {kAction, kBindInteract}, {kAction, kBindL3}};

std::atomic<int> g_mode{kUnset};
std::atomic<std::uint64_t> g_serial{0};
std::mutex g_mutex; // what follows
std::vector<std::pair<std::uint64_t, std::uint64_t>> g_seen; // the glyph images' guest ranges
int g_built_mode = kUnset;
std::uint64_t g_built_serial = 0;
std::vector<std::uint8_t> g_glyphs[kGlyphs], g_spots[kSpots], g_atlas;
stbtt_fontinfo g_font;
bool g_font_ready = false;

int CurrentMode() {
    const int mode = g_mode.load();
    return mode != kUnset ? mode : BbInput::Enabled() ? kKeyboard : kPlayStation;
}

// --- Drawing: straight-alpha RGBA, Elden Ring's keycaps (dark face, light rim, white label) ---

struct Canvas {
    int w, h;
    std::vector<float> px;
    Canvas(int w_, int h_) : w(w_), h(h_), px(std::size_t(w_) * h_ * 4, 0.0f) {}
    void Over(int x, int y, const float* rgb, float a) {
        if (a <= 0.0f || x < 0 || y < 0 || x >= w || y >= h) return;
        float* p = &px[(std::size_t(y) * w + x) * 4];
        const float out = a + p[3] * (1.0f - a);
        for (int c = 0; c < 3; ++c) p[c] = (rgb[c] * a + p[c] * p[3] * (1.0f - a)) / out;
        p[3] = out;
    }
};
// The rim sits on one of BC7 mode 5's four levels between face and label: it encodes exactly.
constexpr float kFace[3] = {22 / 255.0f, 22 / 255.0f, 24 / 255.0f};
constexpr float kLabel[3] = {240 / 255.0f, 240 / 255.0f, 240 / 255.0f};
constexpr float kRim[3] = {(22 + 218 * 43 / 64) / 255.0f, (22 + 218 * 43 / 64) / 255.0f, (24 + 216 * 43 / 64) / 255.0f};

float Cover(float distance) { return std::clamp(0.5f - distance, 0.0f, 1.0f); }
float RoundRect(float x, float y, float cx, float cy, float hw, float hh, float r) {
    const float qx = std::abs(x - cx) - hw + r, qy = std::abs(y - cy) - hh + r;
    return std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f)) + std::min(std::max(qx, qy), 0.0f) - r;
}
// A rounded shape with a rim; `inside` lights its face with the label's white (0..1 per pixel).
template <class Inside>
void Cap(Canvas& cv, float cx, float cy, float hw, float hh, float r, Inside inside) {
    const float rim = std::max(1.0f, (hw + hh) / 16);
    for (int y = 0; y < cv.h; ++y)
        for (int x = 0; x < cv.w; ++x) {
            const float px = x + 0.5f, py = y + 0.5f, d = RoundRect(px, py, cx, cy, hw, hh, r);
            const float a = Cover(d), face = Cover(d + rim);
            if (a <= 0.0f) continue;
            float rgb[3];
            for (int c = 0; c < 3; ++c) rgb[c] = kRim[c] + (kFace[c] - kRim[c]) * (face / a);
            cv.Over(x, y, rgb, a);
            cv.Over(x, y, kLabel, face * inside(px, py));
        }
}

std::vector<int> Codepoints(const std::string& s) {
    std::vector<int> out;
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = s[i];
        const int n = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
        int cp = n == 1 ? c : c & (0x3f >> (n - 1));
        for (int k = 1; k < n && i + k < s.size(); ++k) cp = cp << 6 | (s[i + k] & 0x3f);
        out.push_back(cp);
        i += n;
    }
    return out;
}
float Advance(const std::vector<int>& cps, std::size_t i, float scale) {
    int advance, bearing;
    stbtt_GetCodepointHMetrics(&g_font, cps[i], &advance, &bearing);
    const int kern = i + 1 < cps.size() ? stbtt_GetCodepointKernAdvance(&g_font, cps[i], cps[i + 1]) : 0;
    return (advance + kern) * scale;
}
float TextWidth(const std::vector<int>& cps, float scale) {
    float w = 0;
    for (std::size_t i = 0; i < cps.size(); ++i) w += Advance(cps, i, scale);
    return w;
}
void Text(Canvas& cv, const std::vector<int>& cps, float cx, float baseline, float scale) {
    float x = cx - TextWidth(cps, scale) / 2;
    std::vector<unsigned char> bitmap;
    for (std::size_t i = 0; i < cps.size(); x += Advance(cps, i, scale), ++i) {
        const float shift = x - std::floor(x);
        int x0, y0, x1, y1;
        stbtt_GetCodepointBitmapBoxSubpixel(&g_font, cps[i], scale, scale, shift, 0, &x0, &y0, &x1, &y1);
        if (x1 <= x0 || y1 <= y0) continue;
        bitmap.assign(std::size_t(x1 - x0) * (y1 - y0), 0);
        stbtt_MakeCodepointBitmapSubpixel(&g_font, bitmap.data(), x1 - x0, y1 - y0, x1 - x0, scale, scale, shift, 0,
                                          cps[i]);
        const int ox = int(std::floor(x)) + x0, oy = int(std::lround(baseline)) + y0;
        for (int y = 0; y < y1 - y0; ++y)
            for (int xx = 0; xx < x1 - x0; ++xx)
                cv.Over(ox + xx, oy + y, kLabel, bitmap[std::size_t(y) * (x1 - x0) + xx] / 255.0f);
    }
}

// A keycap `s` pixels square; a label of more than one line ('\n') is set smaller.
void Key(Canvas& cv, float cx, float cy, float s, const std::string& label) {
    Cap(cv, cx, cy, s / 2 - 0.5f, s / 2 - 0.5f, s * 0.2f, [](float, float) { return 0.0f; });
    if (label.empty() || !g_font_ready) return;
    std::vector<std::vector<int>> lines(1);
    for (const int cp : Codepoints(label)) {
        if (cp == '\n') lines.emplace_back();
        else lines.back().push_back(cp);
    }
    // Letters about 60% of the cap, shrunk until the widest line fits inside the rim.
    float size = s * (lines.size() == 1 ? 0.6f : 0.4f);
    for (const auto& line : lines) {
        const float w = TextWidth(line, stbtt_ScaleForPixelHeight(&g_font, size));
        if (w > s * 0.76f) size *= s * 0.76f / w;
    }
    const float scale = stbtt_ScaleForPixelHeight(&g_font, size), cap_height = size * 0.73f, gap = size * 0.95f;
    for (std::size_t i = 0; i < lines.size(); ++i)
        Text(cv, lines[i], cx, cy + cap_height / 2 + (float(i) - (lines.size() - 1) / 2.0f) * gap, scale);
}

// A mouse, the bound button (1 left, 2 right, 3 the wheel) lit; 0 lights none (the camera).
void Mouse(Canvas& cv, float cx, float cy, float s, int button) {
    const float hw = s * 0.27f, hh = s * 0.42f, split = cy - hh * 0.12f, line = std::max(1.0f, s / 24);
    const float wheel_y = cy - hh * 0.55f, wheel_hw = s * 0.05f, wheel_hh = s * 0.11f;
    Cap(cv, cx, cy, hw, hh, hw * 0.95f, [&](float x, float y) {
        const float top = Cover(y - split);
        const float seams = std::max(Cover(std::abs(y - split) - line / 2), top * Cover(std::abs(x - cx) - line / 2));
        const float wheel = Cover(RoundRect(x, y, cx, wheel_y, wheel_hw, wheel_hh, wheel_hw));
        const float lit = button == 1 ? top * Cover(x - cx + line)
                        : button == 2 ? top * Cover(cx + line - x)
                        : button == 3 ? wheel : 0.0f;
        // Seams and an unlit wheel in the rim's grey: 43/64 of the way from face to label.
        return std::max(lit, std::max(seams, wheel) * (43 / 64.0f));
    });
}

std::string KeyName(int scancode) {
    switch (scancode) {
    case 0: return "";
    case SDL_SCANCODE_UP: return "↑";
    case SDL_SCANCODE_DOWN: return "↓";
    case SDL_SCANCODE_LEFT: return "←";
    case SDL_SCANCODE_RIGHT: return "→";
    case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT: return "Shift";
    case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: return "Ctrl";
    case SDL_SCANCODE_LALT: case SDL_SCANCODE_RALT: return "Alt";
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return "Enter";
    case SDL_SCANCODE_BACKSPACE: return "Bksp";
    case SDL_SCANCODE_ESCAPE: return "Esc";
    case SDL_SCANCODE_CAPSLOCK: return "Caps";
    case SDL_SCANCODE_DELETE: return "Del";
    case SDL_SCANCODE_INSERT: return "Ins";
    case SDL_SCANCODE_PAGEUP: return "PgUp";
    case SDL_SCANCODE_PAGEDOWN: return "PgDn";
    default: {
        const char* name = SDL_GetScancodeName(SDL_Scancode(scancode));
        return name && *name ? name : "?";
    }
    }
}
const char* const kMouseLabels[8] = {"", "M1", "M2", "M3", "M4", "M5", "W↑", "W↓"};
// An action's key, else its mouse button's short name.
std::string Label(int action) {
    const int key = host_binding_key(action), mouse = host_binding_mouse(action);
    return key ? KeyName(key) : mouse >= 1 && mouse <= 7 ? kMouseLabels[mouse] : "";
}
// Single keys side by side ("ASD", arrows), longer names apart ("Q Shift").
std::string Join(std::initializer_list<std::string> labels) {
    const bool singles = std::all_of(labels.begin(), labels.end(), [](const std::string& l) { return Codepoints(l).size() <= 1; });
    std::string out;
    for (const std::string& l : labels) out += (out.empty() || singles ? "" : " ") + l;
    return out;
}

void Draw(Canvas& cv, const Face& face, float cx, float cy, float s) {
    switch (face.kind) {
    case kCamera:
        return Mouse(cv, cx, cy, s, 0);
    case kPair:
        return Key(cv, cx, cy, s, Join({Label(face.a), Label(face.b)}));
    case kCluster:
        return Key(cv, cx, cy, s, Label(face.a) + "\n" + Join({Label(face.b), Label(face.c), Label(face.d)}));
    case kAction:
        // A mouse button first, as Elden Ring shows them: the hands are on the mouse.
        if (const int mouse = host_binding_mouse(face.a); mouse >= 1 && mouse <= 3) return Mouse(cv, cx, cy, s, mouse);
        return Key(cv, cx, cy, s, Label(face.a));
    }
}

// --- Encoding: RGBA to the tiled BC7 the game's textures are ---

void Encode(const std::uint8_t* rgba, int w, int h, std::uint8_t* out, int x0, int y0, int blocks_wide) {
    std::uint8_t block[64];
    for (int by = 0; by < h / 4; ++by)
        for (int bx = 0; bx < w / 4; ++bx) {
            for (int i = 0; i < 16; ++i)
                std::memcpy(block + i * 4, rgba + ((std::size_t(by) * 4 + i / 4) * w + bx * 4 + i % 4) * 4, 4);
            BbBc7::EncodeBlock(block, out + BbBc7::TiledBlock(x0 / 4 + bx, y0 / 4 + by, blocks_wide) * 16);
        }
}
std::vector<std::uint8_t> Rgba8(const Canvas& cv) {
    std::vector<std::uint8_t> out(cv.px.size());
    for (std::size_t i = 0; i < out.size(); ++i) out[i] = std::uint8_t(std::lround(std::clamp(cv.px[i], 0.0f, 1.0f) * 255));
    return out;
}

// The glyphs for `mode`, under g_mutex.
void Build(int mode) {
    const std::uint64_t serial = host_bindings_serial();
    if (g_built_mode == mode && (mode != kKeyboard || g_built_serial == serial)) return;
    g_built_mode = mode, g_built_serial = serial;
    if (!g_font_ready) {
        const unsigned char* font = BbOverlay::FontData();
        g_font_ready = stbtt_InitFont(&g_font, font, stbtt_GetFontOffsetForIndex(font, 0)) != 0;
    }
    for (int i = 0; i < kGlyphs; ++i) {
        Canvas cv(32, 32);
        static const char* const xbox[] = {"B", "LB", "LT", "L3", "LS", "↓", "← →", "↑", "↑ ↓",
            "↑\n← ↓ →", "A", "RB", "RT", "R3", "RS", "X", "Y", "Menu", "View", "Menu"};
        if (mode == kXbox) Key(cv, 16, 16, 30, xbox[i]);
        else Draw(cv, kGlyphFaces[i], 16, 16, 30);
        g_glyphs[i].assign(kGlyphSize, 0);
        Encode(Rgba8(cv).data(), 32, 32, g_glyphs[i].data(), 0, 0, 8);
    }
    for (int i = 0; i < kSpots; ++i) {
        Canvas cv(kSpotW, kSpotH);
        static const char* const xbox[] = {"B", "A", "L3"};
        if (mode == kXbox) Key(cv, kSpotW / 2.0f, kSpotH / 2.0f, 26, xbox[i]);
        else Draw(cv, kSpotFaces[i], kSpotW / 2.0f, kSpotH / 2.0f, 26);
        g_spots[i] = Rgba8(cv);
    }
}

void Invalidate() {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> seen;
    {
        std::lock_guard lock{g_mutex};
        seen = g_seen;
    }
    for (const auto& [address, size] : seen) Core::Memory::Instance()->InvalidateMemory(address, size);
}

int PadMode(SDL_JoystickID id) {
    switch (SDL_GetGamepadTypeForID(id)) {
    case SDL_GAMEPAD_TYPE_PS3: case SDL_GAMEPAD_TYPE_PS4: case SDL_GAMEPAD_TYPE_PS5: return kPlayStation;
    default: return kXbox;
    }
}

} // namespace

void Event(const SDL_Event& e) {
    int next = kUnset;
    switch (e.type) {
    case SDL_EVENT_GAMEPAD_ADDED: next = PadMode(e.gdevice.which); break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: next = PadMode(e.gbutton.which); break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (std::abs(e.gaxis.value) > 8000) next = PadMode(e.gaxis.which);
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        if (!SDL_HasGamepad() && BbInput::Enabled()) next = kKeyboard;
        break;
    // The keyboard's prompts follow the PC controls' bindings: without them, no keyboard glyphs.
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_WHEEL:
        if (BbInput::Enabled()) next = kKeyboard;
        break;
    default: break;
    }
    bool changed = next != kUnset && g_mode.exchange(next) != next;
    if (changed) std::printf("Prompts: %s\n", kModeNames[next]);
    // A key rebound on the Key Bindings page redraws its keycap.
    const std::uint64_t serial = host_bindings_serial();
    if (g_serial.exchange(serial) != serial && CurrentMode() == kKeyboard) changed = true;
    if (changed) Invalidate();
}

const std::uint8_t* Replace(std::uint64_t address, std::uint64_t size) {
    if (size != kGlyphSize && size != kAtlasSize) return nullptr;
    // Copy through the runtime's checked mapping path; a stale cache entry must not
    // dereference an unmapped guest address. The result remains owned by this thread.
    thread_local std::vector<std::uint8_t> result;
    result.resize(size);
    Core::Memory::Instance()->CopySparseMemory(address, result.data(), size);
    const auto* guest = result.data();
    const std::uint64_t hash = XXH3_64bits(guest, size);
    int slot = -1;
    if (size == kAtlasSize && hash == kAtlasHash) slot = kGlyphs;
    for (int i = 0; i < kGlyphs && size == kGlyphSize; ++i)
        if (hash == kGlyphHash[i]) slot = i;
    if (slot < 0) return nullptr;

    std::lock_guard lock{g_mutex};
    if (std::find(g_seen.begin(), g_seen.end(), std::pair{address, size}) == g_seen.end()) {
        // Old resource addresses are harmless invalidations, but must not accumulate forever.
        if (g_seen.size() >= 512) g_seen.erase(g_seen.begin());
        g_seen.emplace_back(address, size);
        if (g_seen.size() == 1)
            std::printf("Prompts: the game's button glyphs found, showing %s prompts\n", kModeNames[CurrentMode()]);
    }
    const int mode = CurrentMode();
    if (mode == kPlayStation) return nullptr;
    Build(mode);
    if (slot < kGlyphs) {
        result = g_glyphs[slot];
        return result.data();
    }
    g_atlas.assign(guest, guest + size);
    for (int i = 0; i < kSpots; ++i) Encode(g_spots[i].data(), kSpotW, kSpotH, g_atlas.data(), kSpotX[i], kSpotY, 1024 / 4);
    result = g_atlas;
    return result.data();
}

} // namespace BbPrompts
