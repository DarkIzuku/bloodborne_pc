// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_overlay.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <vector>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <SDL3/SDL.h>
#include "bbport_guest_hooks.h"
#include "bbport_frame_rate.h"
#include "bbport_settings.h"
#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include "vulkan/instance.h"
#include "render/scheduler.h"

// DejaVu Sans (Cyrillic), embedded (third_party/fonts, Bitstream Vera license).
#ifdef _WIN32
#define BB_FONT_SECTION ".section .rdata,\"dr\"\n"
#define BB_FONT_HIDDEN(name) ""
#define BB_FONT_END ".text\n"
#else
#define BB_FONT_SECTION ".section .rodata\n"
#define BB_FONT_HIDDEN(name) ".hidden " name "\n"
#define BB_FONT_END ".previous\n"
#endif
asm(BB_FONT_SECTION
    ".balign 16\n"
    BB_FONT_HIDDEN("bb_font_ttf")
    ".global bb_font_ttf\n"
    "bb_font_ttf:\n"
    ".incbin \"" BB_FONT_PATH "\"\n"
    BB_FONT_HIDDEN("bb_font_ttf_end")
    ".global bb_font_ttf_end\n"
    "bb_font_ttf_end:\n"
    BB_FONT_END);
extern "C" const unsigned char bb_font_ttf[];
extern "C" const unsigned char bb_font_ttf_end[];

extern "C" void runtime_restart(void); // bb-probe (probe.c)
extern "C" __attribute__((weak)) void runtime_toypad_lights(uint32_t out[3]); // runtime_toypad.c
extern "C" int runtime_trophy(int id, const char** name, const char** description, int* grade,
                              int* hidden, int64_t* unlocked_time); // runtime_services.c

namespace BbOverlay {

namespace {

std::mutex imgui_mutex; // the ImGui context: window thread (input) and present thread
bool initialized = false;
std::atomic<bool> menu_open{false};
bool l3_down = false, r3_down = false;
bool dirty = false; // settings changed while open: saved on close
float base_scale = 1.0f;
// The game's text dialog (ImeDialog, the character name), typed on the keyboard: drawn while it
// is open. In fullscreen the window title that showed it is not visible (issues #17, #19).
std::mutex prompt_mutex;
std::atomic<bool> prompt_active{false};
/// A module's line over the game (bbgpu_overlay_banner: BBSC's "joining the host" during its
/// loads, where the game itself shows a black screen).
std::mutex banner_mutex;
std::string banner_text;
std::atomic<bool> banner_active{false};
std::string prompt_title, prompt_text;

// Trophy popups, shown one after another (the platinum follows the last trophy).
struct Banner {
    std::string name;
    int grade;           // 1 bronze .. 4 platinum
    bool sounded = false;
};
std::mutex toast_mutex;
std::deque<Banner> toasts;
std::chrono::steady_clock::time_point toast_since{};
std::atomic<bool> toast_pending{false};
bool trophies_open = false; // the trophy list window, opened from the menu

// Present rate for the FPS counter.
std::chrono::steady_clock::time_point last_present{};
float frame_ms_avg = 0.0f;

float PixelDensity(SDL_WindowID id);

void SetOpen(bool value) {
    if (menu_open.exchange(value) == value) {
        return;
    }
    // The system cursor shows over the menu (window.cpp); ImGui learns where it is now, not at
    // the next motion: mouse motion is not passed on while the menu is closed.
    if (value) {
        if (SDL_Window* window = SDL_GetMouseFocus()) {
            float x = 0.0f, y = 0.0f;
            SDL_GetMouseState(&x, &y);
            const float density = PixelDensity(SDL_GetWindowID(window));
            ImGui::GetIO().AddMousePosEvent(x * density, y * density);
        }
    }
    if (!value && dirty) {
        dirty = false;
        BbSettings::Save();
    }
}

ImGuiKey KeyFromSdl(SDL_Keycode key) {
    switch (key) {
    case SDLK_TAB: return ImGuiKey_Tab;
    case SDLK_LEFT: return ImGuiKey_LeftArrow;
    case SDLK_RIGHT: return ImGuiKey_RightArrow;
    case SDLK_UP: return ImGuiKey_UpArrow;
    case SDLK_DOWN: return ImGuiKey_DownArrow;
    case SDLK_PAGEUP: return ImGuiKey_PageUp;
    case SDLK_PAGEDOWN: return ImGuiKey_PageDown;
    case SDLK_HOME: return ImGuiKey_Home;
    case SDLK_END: return ImGuiKey_End;
    case SDLK_DELETE: return ImGuiKey_Delete;
    case SDLK_BACKSPACE: return ImGuiKey_Backspace;
    case SDLK_SPACE: return ImGuiKey_Space;
    case SDLK_RETURN: return ImGuiKey_Enter;
    case SDLK_KP_ENTER: return ImGuiKey_KeypadEnter;
    case SDLK_ESCAPE: return ImGuiKey_Escape;
    case SDLK_LCTRL: return ImGuiKey_LeftCtrl;
    case SDLK_RCTRL: return ImGuiKey_RightCtrl;
    case SDLK_LSHIFT: return ImGuiKey_LeftShift;
    case SDLK_RSHIFT: return ImGuiKey_RightShift;
    case SDLK_LALT: return ImGuiKey_LeftAlt;
    case SDLK_RALT: return ImGuiKey_RightAlt;
    default: return ImGuiKey_None;
    }
}

ImGuiKey KeyFromGamepad(u8 button) {
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return ImGuiKey_GamepadFaceDown;
    case SDL_GAMEPAD_BUTTON_EAST: return ImGuiKey_GamepadFaceRight;
    case SDL_GAMEPAD_BUTTON_WEST: return ImGuiKey_GamepadFaceLeft;
    case SDL_GAMEPAD_BUTTON_NORTH: return ImGuiKey_GamepadFaceUp;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return ImGuiKey_GamepadDpadUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return ImGuiKey_GamepadDpadDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return ImGuiKey_GamepadDpadLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return ImGuiKey_GamepadDpadRight;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return ImGuiKey_GamepadL1;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return ImGuiKey_GamepadR1;
    case SDL_GAMEPAD_BUTTON_START: return ImGuiKey_GamepadStart;
    case SDL_GAMEPAD_BUTTON_BACK: return ImGuiKey_GamepadBack;
    default: return ImGuiKey_None;
    }
}

float PixelDensity(SDL_WindowID id) {
    SDL_Window* window = SDL_GetWindowFromID(id);
    const float density = window ? SDL_GetWindowPixelDensity(window) : 1.0f;
    return density > 0.0f ? density : 1.0f;
}

// Marks the settings dirty when a widget changed them.
template <typename T>
void Store(std::atomic<T>& target, T value, bool changed) {
    if (changed) {
        target = value;
        dirty = true;
    }
}

void Checkbox(const char* label, std::atomic<bool>& value) {
    bool v = value;
    Store(value, v, ImGui::Checkbox(label, &v));
}

void Slider(const char* label, std::atomic<float>& value, float lo, float hi) {
    float v = value;
    Store(value, v, ImGui::SliderFloat(label, &v, lo, hi, "%.2f"));
}

void Hint(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// The USB toy pad: the figures on the emulated pad, put on and lifted off from
// the menu with the gamepad. The runtime names the figures file (BB_TOYPAD_FILE) once the game
// opens the pad and reads it again when it changes; this section edits that file.
struct ToyPadFigure {
    int zone;
    unsigned id;
};
struct ToyPadName {
    unsigned id;
    std::string name;
};

std::vector<ToyPadFigure> ReadToyPad(const char* path) {
    std::vector<ToyPadFigure> figures;
    FILE* file = std::fopen(path, "r");
    char line[256];
    while (file && std::fgets(line, sizeof(line), file)) {
        if (char* hash = std::strchr(line, '#')) {
            *hash = 0;
        }
        int zone;
        unsigned id;
        if (std::sscanf(line, "%d %u", &zone, &id) == 2 && zone >= 1 && zone <= 3 && id) {
            figures.push_back({zone, id});
        }
    }
    if (file) {
        std::fclose(file);
    }
    return figures;
}

// Figure names: "<id> <name>" lines in figure_ids.txt next to the figures file (BB_TOYPAD_NAMES
// names another one); without it the ids are shown.
const std::vector<ToyPadName>& ToyPadNames(const char* figures_path) {
    static std::vector<ToyPadName> names;
    static bool loaded;
    if (!loaded) {
        loaded = true;
        std::string path;
        if (const char* env = std::getenv("BB_TOYPAD_NAMES"); env && *env) {
            path = env;
        } else {
            path = figures_path;
            const size_t slash = path.rfind('/');
            path = (slash == std::string::npos ? std::string{} : path.substr(0, slash + 1)) + "figure_ids.txt";
        }
        FILE* file = std::fopen(path.c_str(), "r");
        char line[256];
        while (file && std::fgets(line, sizeof(line), file)) {
            unsigned id;
            int used = 0;
            if (std::sscanf(line, "%u %n", &id, &used) == 1 && id) {
                std::string name = line + used;
                while (!name.empty() && (name.back() == '\n' || name.back() == '\r' || name.back() == ' ')) {
                    name.pop_back();
                }
                names.push_back({id, name});
            }
        }
        if (file) {
            std::fclose(file);
        }
    }
    return names;
}

std::string ToyPadLabel(const std::vector<ToyPadName>& names, unsigned id) {
    for (const auto& n : names) {
        if (n.id == id) {
            return n.name + " (" + std::to_string(id) + ")";
        }
    }
    return "#" + std::to_string(id);
}

void WriteToyPad(const char* path, const std::vector<ToyPadFigure>& figures,
                 const std::vector<ToyPadName>& names) {
    const std::string temp = std::string{path} + ".tmp";
    FILE* file = std::fopen(temp.c_str(), "w");
    if (!file) {
        return;
    }
    std::fputs("# Figures on the virtual Toy Pad: \"<zone> <figure id>\", one per line.\n"
               "# Zone 1: centre (1 figure), 2: left (3), 3: right (3). Also edited from the menu "
               "(Insert / L3+R3).\n", file);
    for (const auto& f : figures) {
        std::fprintf(file, "%d %u  # %s\n", f.zone, f.id, ToyPadLabel(names, f.id).c_str());
    }
    std::fclose(file);
    std::rename(temp.c_str(), path);
}

void ToyPadSection() {
    const char* path = std::getenv("BB_TOYPAD_FILE");
    if (!path || !*path) {
        return;
    }
    const auto& names = ToyPadNames(path);
    auto figures = ReadToyPad(path);
    bool changed = false;
    u32 lights[3] = {};
    if (runtime_toypad_lights) {
        runtime_toypad_lights(lights);
    }
    static unsigned pick_id = 1; // the figure to put on
    ImGui::SeparatorText(BbSettings::MenuText("Toy Pad", "Toy Pad (фигурки)"));
    // Choosing a figure: characters, then vehicles and gadgets (ids from 1000).
    const std::string current = ToyPadLabel(names, pick_id);
    if (ImGui::BeginCombo(BbSettings::MenuText("Figure", "Фигурка"), current.c_str(),
                          ImGuiComboFlags_HeightLarge)) {
        for (int vehicles = 0; vehicles < 2; ++vehicles) {
            ImGui::SeparatorText(vehicles ? BbSettings::MenuText("Vehicles and gadgets", "Техника")
                                          : BbSettings::MenuText("Characters", "Персонажи"));
            for (const auto& n : names) {
                if ((n.id >= 1000) != bool(vehicles)) {
                    continue;
                }
                const std::string label = ToyPadLabel(names, n.id);
                if (ImGui::Selectable(label.c_str(), n.id == pick_id)) {
                    pick_id = n.id;
                }
                if (n.id == pick_id && ImGui::IsWindowAppearing()) {
                    ImGui::SetScrollHereY();
                }
            }
        }
        ImGui::EndCombo();
    }
    // The zones in the pad's order (left, centre, right), each lit as the game lights it: the
    // zone it wants a figure on glows.
    static const int order[] = {2, 1, 3};
    static const char* zone_en[] = {"Centre", "Left", "Right"};
    static const char* zone_ru[] = {"Центр", "Левая", "Правая"};
    for (const int zone : order) {
        ImGui::PushID(zone);
        const u32 c = lights[zone - 1];
        const ImVec4 colour = c ? ImVec4(((c >> 16) & 255) / 255.0f, ((c >> 8) & 255) / 255.0f,
                                         (c & 255) / 255.0f, 1.0f)
                                : ImVec4(0.35f, 0.35f, 0.35f, 1.0f);
        ImGui::TextColored(colour, "%s", c ? "●" : "○");
        ImGui::SameLine();
        ImGui::TextUnformatted(BbSettings::MenuText(zone_en[zone - 1], zone_ru[zone - 1]));
        int count = 0;
        for (size_t i = 0; i < figures.size(); ++i) {
            if (figures[i].zone != zone) {
                continue;
            }
            ++count;
            ImGui::PushID(int(i));
            ImGui::SameLine();
            const std::string label = ToyPadLabel(names, figures[i].id) + "  ×";
            if (ImGui::SmallButton(label.c_str())) {
                figures.erase(figures.begin() + long(i));
                changed = true;
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
        const bool full = count >= (zone == 1 ? 1 : 3) || figures.size() >= 7;
        if (!full) {
            ImGui::SameLine();
            if (ImGui::SmallButton(BbSettings::MenuText("+ put here", "+ поставить сюда"))) {
                figures.push_back({zone, pick_id});
                changed = true;
            }
        }
        ImGui::PopID();
    }
    Hint(BbSettings::MenuText(
        "Choose a figure, then \"+ put here\" on a zone; press a figure to lift it. The dot is the "
        "zone's light: when the game asks for a figure it lights the zone to put it on. If the "
        "game misses a figure, lift it and put it on again.",
        "Выберите фигурку и нажмите «+ поставить сюда» у зоны; нажатие на фигурку снимает её. "
        "Кружок — подсветка зоны: когда игра просит фигурку, она подсвечивает зону, куда её "
        "ставить. Если игра не заметила фигурку — снимите и поставьте снова."));
    if (changed) {
        WriteToyPad(path, figures, names);
    }
}

// The settings menu as in 0.3 and 0.4: one window (moved with the mouse, its place kept in
// bbport.ini), sections one under the other, ImGui's own widgets and keyboard/gamepad navigation.
void Menu() {
    auto& s = BbSettings::Get();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    // Where it was moved last (bbport.ini menu_pos, a fraction of the screen), kept on screen.
    ImVec2 pos(viewport->WorkPos.x + 40.0f * base_scale, viewport->WorkPos.y + 40.0f * base_scale);
    if (s.menu_x >= 0.0f && s.menu_y >= 0.0f) {
        const float margin = 80.0f * base_scale;
        pos.x = viewport->WorkPos.x +
                std::clamp(s.menu_x * viewport->WorkSize.x, 0.0f, std::max(viewport->WorkSize.x - margin, 0.0f));
        pos.y = viewport->WorkPos.y +
                std::clamp(s.menu_y * viewport->WorkSize.y, 0.0f, std::max(viewport->WorkSize.y - margin, 0.0f));
    }
    ImGui::SetNextWindowPos(pos, ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(ImVec2(620.0f * base_scale, 0.0f), ImGuiCond_Appearing);
    bool keep_open = true;
    if (!ImGui::Begin(
            BbSettings::MenuText("Bloodborne - Graphics  (Insert / L3+R3)###bbport_settings",
                                 "Bloodborne — настройки  (Insert / L3+R3)###bbport_settings"),
            &keep_open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    // Moved: remembered (saved with the settings when the menu closes).
    if (!ImGui::IsWindowAppearing() && viewport->WorkSize.x > 0.0f && viewport->WorkSize.y > 0.0f) {
        const ImVec2 at = ImGui::GetWindowPos();
        const float fx = (at.x - viewport->WorkPos.x) / viewport->WorkSize.x;
        const float fy = (at.y - viewport->WorkPos.y) / viewport->WorkSize.y;
        if (std::abs(at.x - pos.x) >= 1.0f || std::abs(at.y - pos.y) >= 1.0f) {
            s.menu_x = fx;
            s.menu_y = fy;
            dirty = true;
        }
    }
    // Always readable, even when the rest of the menu is in Russian.
    static const char* languages[] = {"English", "Русский"};
    int language = s.menu_language == BbSettings::MenuLanguage::Russian ? 1 : 0;
    if (ImGui::Combo("Language", &language, languages, 2)) {
        s.menu_language =
            language == 1 ? BbSettings::MenuLanguage::Russian : BbSettings::MenuLanguage::English;
        s.menu_language_chosen = true;
        BbSettings::Save();
    }
    ImGui::Text(BbSettings::MenuText("%.0f FPS  (%.1f ms)", "%.0f FPS  (%.1f мс)"),
                frame_ms_avg > 0.0f ? 1000.0f / frame_ms_avg : 0.0f, frame_ms_avg);
    ToyPadSection();

    ImGui::SeparatorText(BbSettings::MenuText("Temporal upscaler", "Временной апскейлер"));
    const char* upscalers[] = {
        BbSettings::MenuText("Off", "Выкл"), "FSR 3.1", "FSR 4 (INT8)", "FSR 4.1.1",
        BbSettings::MenuText("TAA (native anti-aliasing)", "TAA (нативное сглаживание)"),
        "DLSS (NVIDIA RTX)"};
    static_assert(sizeof(upscalers) / sizeof(upscalers[0]) == BbSettings::UpscalerCount);
    static const char* later[] = {"XeSS"};
    int upscaler = s.upscaler;
    if (ImGui::BeginCombo(BbSettings::MenuText("Upscaler", "Апскейлер"), upscalers[upscaler])) {
        for (int i = 0; i < BbSettings::UpscalerCount; ++i) {
            const bool supported = i == BbSettings::UpscalerFsr4     ? s.fsr4_supported.load()
                                   : i == BbSettings::UpscalerFsr411 ? s.fsr411_supported.load()
                                   : i == BbSettings::UpscalerDlss   ? s.dlss_supported.load()
                                                                     : true;
            ImGui::BeginDisabled(!supported);
            if (ImGui::Selectable(upscalers[i], i == upscaler)) {
                Store(s.upscaler, i, true);
            }
            ImGui::EndDisabled();
            if (!supported) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", BbSettings::MenuText("— not supported by this GPU",
                                                         "— не поддерживается видеокартой"));
            }
        }
        for (const char* name : later) {
            ImGui::BeginDisabled();
            ImGui::Selectable(name, false);
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled("%s", BbSettings::MenuText("— in development", "— в работе"));
        }
        ImGui::EndCombo();
    }
    if (s.upscaler == BbSettings::UpscalerDlss) {
        Hint(BbSettings::MenuText(
            "NVIDIA DLSS Super Resolution (RTX GPUs; Native AA is DLAA). Needs the DLSS bridge "
            "and NVIDIA's library next to the game; otherwise FSR 3.1 is used.",
            "NVIDIA DLSS Super Resolution (видеокарты RTX; Native AA — это DLAA). Нужны мост DLSS "
            "и библиотека NVIDIA рядом с игрой; без них используется FSR 3.1."));
    }
    const auto problem_text = [](const char* text) {
        ImGui::PushTextWrapPos();
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "%s", text);
        ImGui::PopTextWrapPos();
    };
    if (const char* problem = s.dlss_problem.load(); problem && s.upscaler == BbSettings::UpscalerDlss) {
        char line[192];
        std::snprintf(line, sizeof(line), "DLSS: %s", problem);
        problem_text(line);
    }
    if (const char* problem = s.fsr4_problem.load()) {
        char line[192];
        std::snprintf(line, sizeof(line),
                      BbSettings::MenuText("FSR 4 unavailable: %s", "FSR 4 недоступен: %s"), problem);
        problem_text(line);
        if (!BbSettings::IsFsr4(s.upscaler)) {
            ImGui::PushTextWrapPos();
            ImGui::TextUnformatted(BbSettings::MenuText(
                "The mode selected above is active. You can select FSR 4 again.",
                "Активен режим, выбранный выше. FSR 4 можно выбрать снова."));
            ImGui::PopTextWrapPos();
        }
    }
    if (BbSettings::IsFsr4(s.upscaler)) {
        if (s.upscaler == BbSettings::UpscalerFsr411) {
            Hint(BbSettings::MenuText(
                "FSR 4.1.1: the model from AMD's 4.1.1 DLL, reproduced in Vulkan (output matches "
                "the DLL). Assets are built from your DLL in the launcher.",
                "FSR 4.1.1: модель из DLL AMD 4.1.1, воспроизведённая в Vulkan (результат совпадает "
                "с DLL). Ассеты собираются из вашей DLL в лаунчере."));
        } else {
            Hint(BbSettings::MenuText(
                "FSR 4 in INT8 mode (v07 model from AMD FidelityFX SDK sources). Higher quality "
                "than FSR 3.1, but the pass is more demanding. Changing the preset rebuilds the "
                "model (a brief pause).",
                "FSR 4 в режиме INT8 (модель v07 из исходников AMD FidelityFX SDK). Качество выше, "
                "чем у FSR 3.1, но проход тяжелее. Смена пресета пересобирает модель (короткая "
                "пауза)."));
        }
        Checkbox(BbSettings::MenuText("FSR 4: auto exposure", "FSR 4: авто-экспозиция"),
                 s.fsr4_auto_exposure);
        Checkbox(BbSettings::MenuText("FSR 4: invert jitter sign", "FSR 4: обратный знак jitter"),
                 s.fsr4_invert_jitter);
        Hint(BbSettings::MenuText(
            "For diagnosing ghosting: the FSR 4 network normalizes color by exposure and uses "
            "it to decide when to discard previous frames. Changes apply immediately, without a "
            "restart.",
            "Проверка при гостинге: сеть FSR 4 нормирует цвет по экспозиции и по ней решает, "
            "когда отбросить прошлые кадры. Меняются сразу, без перезапуска."));
    }
    const bool upscaler_on = s.upscaler != BbSettings::UpscalerOff;
    const bool taa = s.upscaler == BbSettings::UpscalerTaa;
    ImGui::BeginDisabled(!upscaler_on);
    ImGui::BeginDisabled(taa);
    int preset = taa ? BbSettings::NativeAA : s.preset.load();
    char preset_label[64];
    std::snprintf(preset_label, sizeof(preset_label), "%s (x%.1f)", BbSettings::PresetName(preset),
                  BbSettings::PresetScale(preset));
    if (ImGui::BeginCombo(BbSettings::MenuText("Preset", "Пресет"), preset_label)) {
        for (int i = 0; i < BbSettings::PresetCount; ++i) {
            char label[64];
            const float scale = BbSettings::PresetScale(i);
            const auto output = BbSettings::CurrentOutput();
            std::snprintf(
                label, sizeof(label),
                BbSettings::MenuText("%s (x%.1f, render %dx%d)", "%s (x%.1f, рендер %dx%d)"),
                BbSettings::PresetName(i), scale, int(std::lround(output.width / scale / 2) * 2),
                int(std::lround(output.height / scale / 2) * 2));
            if (ImGui::Selectable(label, i == preset)) {
                Store(s.preset, i, true);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    if (taa) {
        ImGui::TextWrapped("%s", BbSettings::MenuText(
            "TAA anti-aliases the scene at output resolution, without an FSR model or upscaling. "
            "Your saved FSR preset is restored when you select FSR.",
            "TAA сглаживает сцену в разрешении вывода, без модели FSR и апскейлинга. "
            "Сохранённый пресет FSR восстановится при выборе FSR."));
    }
    ImGui::Text(
        BbSettings::MenuText("Active scene render: %d x %d", "Активный рендер сцены: %d x %d"),
        s.active_render_width.load(), s.active_render_height.load());
    if (BbSettings::FixedRenderSession()) {
        ImGui::Text(BbSettings::MenuText("Startup preset: %s", "Пресет при запуске: %s"),
                    BbSettings::PresetName(s.startup_preset));
        if (const char* automatic = std::getenv("BB_AUTO_RENDER_RES");
            automatic && automatic[0] == '1') {
            Hint(BbSettings::MenuText(
                "When output is not 1080p, the entire game renders at the preset resolution "
                "(startup patch): this is fastest on Steam Deck and weaker GPUs. Preset or output "
                "resolution changes require a restart. Enable Live resolution changes below to "
                "change them without restarting (post-processing then stays at 1080p, which is "
                "slower).",
                "При выводе не 1080p вся игра рисуется в разрешении пресета (патч при запуске): "
                "это быстрее всего на Steam Deck и слабых GPU. Смена пресета или разрешения "
                "вывода — после перезапуска. Пункт «Смена разрешения на лету» ниже включает "
                "смену без перезапуска (постобработка тогда остаётся в 1080p, медленнее)."));
        } else {
            Hint(BbSettings::MenuText(
                "BB_RENDER_RES fixes the scene size at startup. Remove this explicit environment "
                "variable to change resolution and presets without restarting the game.",
                "BB_RENDER_RES фиксирует размер сцены при запуске. Уберите эту явную переменную "
                "для смены разрешения и пресетов без перезапуска игры."));
        }
    } else {
        Hint(BbSettings::MenuText(
            "Native AA: the upscaler acts as anti-aliasing. Other presets reduce the scene render "
            "resolution relative to the output. The UI renders at output resolution. "
            "The preset applies from the next frame without restarting the game.",
            "Native AA: апскейлер работает как сглаживание. Остальные пресеты уменьшают разрешение "
            "отрисовки сцены относительно вывода. Интерфейс рисуется в разрешении вывода. "
            "Пресет применяется со следующего кадра без перезапуска игры."));
    }
    Checkbox(BbSettings::MenuText("Sharpening (RCAS)", "Резкость (RCAS)"), s.sharpen);
    ImGui::BeginDisabled(!s.sharpen);
    Slider(BbSettings::MenuText("Sharpness", "Сила резкости"), s.sharpness, 0.0f, 2.0f);
    Hint(BbSettings::MenuText(
        "Up to 1: the upscaler's own sharpening (RCAS). Above 1 adds another RCAS pass. "
        "Ctrl+click the slider to enter an exact value.",
        "До 1 — резкость самого апскейлера (RCAS). Выше 1 добавляется ещё один проход RCAS. "
        "Ctrl+клик по ползунку — ввести точное значение."));
    ImGui::EndDisabled();
    // Frame generation: hidden until its artifacts are fixed (BB_FRAME_GEN=1 for tests).
    if (std::getenv("BB_FRAME_GEN")) {
        ImGui::BeginDisabled(!s.frame_generation_supported.load(std::memory_order_relaxed));
        Checkbox(BbSettings::MenuText("Frame generation (FSR 3)", "Генерация кадров (FSR 3)"),
                 s.frame_generation);
        ImGui::EndDisabled();
        if (const char* problem = s.frame_generation_problem.load(std::memory_order_relaxed)) {
            Hint(problem);
        } else {
            Hint(BbSettings::MenuText(
                "Shows a frame interpolated by FSR 3 between each two of the game's: twice the frames "
                "on screen, a little more input lag. Works with every upscaler preset except Off.",
                "Между каждыми двумя кадрами игры показывается кадр, интерполированный FSR 3: на "
                "экране вдвое больше кадров, задержка ввода чуть выше. Работает со всеми пресетами "
                "апскейлера, кроме «Выкл»."));
        }
    }
    Checkbox(BbSettings::MenuText("Subpixel jitter", "Субпиксельный сдвиг (jitter)"), s.jitter);
    Hint(BbSettings::MenuText(
        "Each frame shifts the scene by a fraction of a pixel, letting the upscaler reconstruct "
        "more detail from multiple frames. Without it, only history-based anti-aliasing remains.",
        "Каждый кадр сцена сдвигается на долю пикселя, и апскейлер собирает из нескольких "
        "кадров больше деталей. Без него получается только сглаживание по истории."));

    ImGui::SeparatorText(BbSettings::MenuText("Reactive mask", "Маска реактивности"));
    ImGui::BeginDisabled(taa);
    Checkbox(BbSettings::MenuText("Enable mask", "Включить маску"), s.reactive);
    Hint(BbSettings::IsFsr4(s.upscaler)
             ? BbSettings::MenuText(
                   "Marks transparent effects (haze, light, water, particles). FSR 4 takes no "
                   "mask itself: its output is blended with the current frame there, so the "
                   "effects do not drag previous frames along.",
                   "Помечает прозрачные эффекты (дымку, свет, воду, частицы). FSR 4 сам маску не "
                   "принимает: там его результат смешивается с текущим кадром, и эффекты не "
                   "тянут за собой прошлые кадры.")
             : BbSettings::MenuText(
                   "Marks transparent effects (particles, haze) so the upscaler relies less on "
                   "previous frames. Reduces trails behind effects, but shimmering returns "
                   "underneath them.",
                   "Помечает прозрачные эффекты (частицы, дымку), чтобы апскейлер меньше "
                   "опирался на прошлые кадры. Меньше шлейфов за эффектами, но под ними "
                   "возвращается дрожание."));
    ImGui::BeginDisabled(!s.reactive);
    Slider(BbSettings::MenuText("Scale", "Масштаб"), s.reactive_scale, 0.0f, 4.0f);
    Slider(BbSettings::MenuText("Threshold", "Порог"), s.reactive_threshold, 0.0f, 1.0f);
    Slider(BbSettings::MenuText("Maximum", "Максимум"), s.reactive_max, 0.0f, 1.0f);
    bool show_mask = s.debug_view == BbSettings::DebugReactive;
    if (ImGui::Checkbox(BbSettings::MenuText("Show mask (debug)", "Показать маску (отладка)"),
                        &show_mask)) {
        s.debug_view = show_mask ? BbSettings::DebugReactive : BbSettings::DebugNone;
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    Checkbox(BbSettings::MenuText("Character motion vectors", "Векторы движения персонажей"),
             s.object_motion);
    Hint(BbSettings::MenuText(
        "Accurate vectors for animated objects: clothing and weapons break up less "
        "during movement. The static scene does not get an additional pass. "
        "Changes apply after restarting the game.",
        "Точные векторы для анимированных объектов: одежда и оружие меньше рассыпаются "
        "при движении. Статичная сцена не получает дополнительный проход. "
        "Изменение применяется после перезапуска игры."));
    bool show_motion = s.debug_view == BbSettings::DebugMotion;
    if (ImGui::Checkbox(BbSettings::MenuText("Show motion vectors (debug)",
                                             "Показать векторы движения (отладка)"),
                        &show_motion)) {
        s.debug_view = show_motion ? BbSettings::DebugMotion : BbSettings::DebugNone;
    }
    Hint(BbSettings::MenuText(
        "Red/green: horizontal/vertical motion (8 pixels = full brightness). "
        "Blue: the pixel received an accurate object vector, not just camera motion. "
        "The upscaler treats a moving object with no blue or red/green as "
        "stationary, causing trails.",
        "Красный/зелёный: движение по горизонтали/вертикали (8 пикселей = полная яркость). "
        "Синий: пиксель получил точный вектор объекта, а не только движение камеры. "
        "Движущийся предмет без синего и без красного/зелёного апскейлер считает "
        "неподвижным, отсюда шлейф."));
    ImGui::EndDisabled(); // upscaler off

    ImGui::SeparatorText(BbSettings::MenuText("Output", "Вывод"));
    // The aspect ratio first; its resolutions follow (another one keeps the height).
    const auto aspect_label = [](int a) {
        if (a != BbSettings::AspectAuto) return std::string(BbSettings::AspectKey(a));
        return std::string(BbSettings::MenuText("Auto", "Авто")) + " (" +
               BbSettings::AspectKey(BbSettings::ResolvedAspect(a)) + ")";
    };
    const int aspect = s.aspect;
    if (ImGui::BeginCombo(BbSettings::MenuText("Aspect ratio", "Соотношение сторон"),
                          aspect_label(aspect).c_str())) {
        for (int a = 0; a < BbSettings::AspectCount; ++a) {
            if (ImGui::Selectable(aspect_label(a).c_str(), a == aspect)) {
                Store(s.output_res, BbSettings::PackSize(BbSettings::NearestOutput(a, BbSettings::CurrentOutput())),
                      true);
                Store(s.aspect, a, true);
            }
        }
        ImGui::EndCombo();
    }
    const auto size_label = [](BbSettings::OutputSize o) {
        return std::to_string(o.width) + " x " + std::to_string(o.height);
    };
    const auto current = BbSettings::CurrentOutput();
    if (ImGui::BeginCombo(BbSettings::MenuText("Resolution", "Разрешение"), size_label(current).c_str())) {
        for (const auto& o : BbSettings::Outputs(s.aspect)) {
            if (ImGui::Selectable(size_label(o).c_str(), o == current)) {
                Store(s.output_res, BbSettings::PackSize(o), true);
            }
        }
        ImGui::EndCombo();
    }
    if (BbSettings::FixedRenderSession()) {
        Hint(BbSettings::MenuText(
            "Size of the final frame and UI. The preset sets the scene size relative to "
            "the output: 4K Performance = 1920x1080. Another aspect ratio widens the view (the "
            "UI keeps to the screen edges). Applies after restarting the game.",
            "Размер готового кадра и интерфейса. Пресет задаёт размер сцены относительно "
            "вывода: 4K Performance = 1920x1080. Другое соотношение сторон расширяет обзор "
            "(интерфейс у краёв экрана). Применяется после перезапуска игры."));
    } else {
        Hint(BbSettings::MenuText(
            "The final frame and UI size changes at the next frame boundary. "
            "The preset sets the scene size relative to the output: 4K Performance = 1920x1080. "
            "Another aspect ratio widens the view (the UI keeps to the screen edges). "
            "Changing the size resets the upscaler's history and may cause a brief pause.",
            "Размер готового кадра и интерфейса меняется на границе следующего кадра. "
            "Пресет задаёт размер сцены относительно вывода: 4K Performance = 1920x1080. "
            "Другое соотношение сторон расширяет обзор (интерфейс у краёв экрана). "
            "Смена размера сбрасывает историю апскейлера и может вызвать короткую паузу."));
    }
    const char* live_modes[] = {BbSettings::MenuText("Auto (based on GPU)", "Авто (по видеокарте)"),
                                BbSettings::MenuText("Off (faster)", "Выключена (быстрее)"),
                                BbSettings::MenuText("On", "Включена")};
    int live = s.live_resolution + 1;
    if (ImGui::BeginCombo(
            BbSettings::MenuText("Live resolution changes", "Смена разрешения на лету"),
            live_modes[live])) {
        for (int i = 0; i < 3; ++i) {
            if (ImGui::Selectable(live_modes[i], i == live)) {
                Store(s.live_resolution, i - 1, true);
            }
        }
        ImGui::EndCombo();
    }
    if (BbSettings::FixedRenderSession() && s.startup_live_resolution != 0) {
        // Started with a fixed render size (BB_LIVE_RES=0 or BB_RENDER_RES): the setting is
        // not what this session runs.
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s",
                           BbSettings::MenuText("This session started with a fixed resolution: "
                                                "changes apply after a restart",
                                                "Эта сессия запущена с фиксированным разрешением: "
                                                "смена — после перезапуска"));
    }
    Hint(BbSettings::MenuText(
        "On: output resolution and preset change without restarting, but game post-processing "
        "stays at 1080p, which is noticeably slower on Steam Deck and older GPUs. "
        "Off: everything renders at the preset resolution, and changes require a restart. Auto "
        "enables this on powerful discrete GPUs. Applies after restarting the game.",
        "Включена: разрешение вывода и пресет меняются без перезапуска, но постобработка игры "
        "остаётся в 1080p — на Steam Deck и старых видеокартах это заметно медленнее. "
        "Выключена: всё рисуется в разрешении пресета, смена — через перезапуск. Авто включает "
        "её на мощных дискретных видеокартах. Применяется после перезапуска игры."));
    ImGui::SeparatorText(BbSettings::MenuText("Game effects",
                                              "Эффекты игры (после перезапуска)"));
    const char* lods[] = {BbSettings::MenuText("Highest (-2)", "Максимальная (-2)"),
                          BbSettings::MenuText("Game default", "Как в игре"),
                          BbSettings::MenuText("Lower (1)", "Ниже (1)"),
                          BbSettings::MenuText("Lowest (2)", "Минимальная (2)")};
    static constexpr int lod_values[] = {-2, 0, 1, 2};
    int lod_index = 1;
    for (int i = 0; i < 4; ++i) {
        if (lod_values[i] == s.model_lod) lod_index = i;
    }
    if (ImGui::BeginCombo(BbSettings::MenuText("Model detail", "Детализация моделей"),
                          lods[lod_index])) {
        for (int i = 0; i < 4; ++i) {
            if (ImGui::Selectable(lods[i], i == lod_index)) {
                Store(s.model_lod, lod_values[i], true);
            }
        }
        ImGui::EndCombo();
    }
    for (int e = 0; e < BbSettings::EffectCount; ++e) {
        const auto& effect = BbSettings::Effects[e];
        Checkbox(BbSettings::MenuText(effect.label, effect.label_ru), s.effects[e]);
        if (std::string_view(effect.key) == "debug_camera") {
            Hint(BbSettings::MenuText("Hold Cross and press L3 (keyboard: F + Left Ctrl).",
                                      "Удерживайте Cross и нажмите L3 (клавиатура: F + Left Ctrl)."));
        } else if (std::string_view(effect.key) == "debug_menu") {
            Hint(BbSettings::MenuText(
                "Left side of the touchpad (Tab); right side: Backspace. Needs the adhoc folder "
                "from Nexus mod #253 (the fonts in adhoc/font).",
                "Левая сторона тачпада (Tab), правая — Backspace. Нужна папка adhoc из мода "
                "Nexus #253 (шрифты в adhoc/font)."));
        }
    }
    BbGuestHooks::ApplyGameEffects();
    Hint(BbSettings::MenuText(
        "SSR reflections, intros, free camera and debug menu apply after a restart. Other effects "
        "switch immediately. Motion blur and shadows from dynamic lights place a significant load on the GPU.",
        "Отражения SSR, заставки, свободная камера и debug menu применяются после перезапуска. "
        "Остальные эффекты переключаются сразу. Размытие в движении и тени от динамических источников заметно нагружают GPU."));

    bool restart =
        s.object_motion != s.startup_object_motion || s.model_lod != s.startup_model_lod ||
        s.live_resolution != s.startup_live_resolution || BbSettings::ResolutionNeedsRestart();
    for (int e = 0; e < BbSettings::EffectCount; ++e) {
        restart |= !BbSettings::IsHotEffect(e) && s.effects[e] != s.startup_effects[e];
    }
    if (restart) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s",
                           BbSettings::MenuText("Changes apply after restarting the game",
                                                "Изменения применятся после перезапуска игры"));
        if (ImGui::Button(
                BbSettings::MenuText("Apply and restart game", "Применить и перезапустить игру"))) {
            BbSettings::Save();
            runtime_restart();
        }
    }

    ImGui::SeparatorText(BbSettings::MenuText("Frame rate limit (now)", "Лимит кадров (сейчас)"));
    {
        static const int steps[] = {0, 30, 60, 90, 120, 144, 240};
        static const char* labels[] = {"As launched", "30", "60", "90", "120", "144", "240"};
        static const char* labels_ru[] = {"Как при запуске", "30", "60", "90", "120", "144", "240"};
        int index = 0;
        for (int i = 0; i < 7; ++i) {
            if (steps[i] == BbFrameRate::Limit()) index = i;
        }
        if (ImGui::BeginCombo(BbSettings::MenuText("Limit (F3: 30 FPS on and off)", "Лимит (F3: 30 FPS вкл/выкл)"),
                              BbSettings::MenuText(labels[index], labels_ru[index]))) {
            for (int i = 0; i < 7; ++i) {
                if (ImGui::Selectable(BbSettings::MenuText(labels[i], labels_ru[i]), i == index)) {
                    BbFrameRate::SetLimit(steps[i]);
                }
            }
            ImGui::EndCombo();
        }
        Hint(BbSettings::MenuText(
            "Not saved. At 30 FPS the cinematic of Laurence (optional boss) does not hang, as it can "
            "above 30 (the community 60 FPS patch has the same problem).",
            "Не сохраняется. На 30 FPS кинематика Лоуренса (необязательный босс) не зависает, "
            "как может выше 30 (у общего патча на 60 FPS та же проблема)."));
    }
    ImGui::SeparatorText(BbSettings::MenuText("Other", "Прочее"));
    Checkbox(BbSettings::MenuText("FPS counter in corner", "Счётчик FPS в углу"), s.show_fps);

    ImGui::Spacing();
    if (ImGui::Button(BbSettings::MenuText("Close", "Закрыть"))) {
        keep_open = false;
    }
    // Issues #74, #116: leaving the game from the pad (this menu opens with L3+R3). A second
    // press confirms; the window thread then closes like the window's own close button.
    static bool quit_armed;
    ImGui::SameLine();
    if (ImGui::Button(quit_armed ? BbSettings::MenuText("Press again to quit", "Нажмите ещё раз для выхода")
                                 : BbSettings::MenuText("Quit game", "Выйти из игры"))) {
        if (quit_armed) {
            SDL_Event quit{};
            quit.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&quit);
        }
        quit_armed = !quit_armed;
    }
    if (quit_armed && !ImGui::IsItemFocused() && !ImGui::IsItemHovered()) {
        quit_armed = false;
    }
    if (ImGui::Button(BbSettings::MenuText("Trophies", "Трофеи"))) {
        trophies_open = !trophies_open;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", BbSettings::MenuText("Settings are saved to bbport.ini",
                                             "Настройки сохраняются в bbport.ini"));
    ImGui::End();
    if (!keep_open) {
        SetOpen(false);
    }
}

void FpsCounter() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale;
    ImGui::SetNextWindowPos(
        ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - pad, viewport->WorkPos.y + pad),
        ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.5f);
    ImGui::Begin("##fps", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    const auto& s = BbSettings::Get();
    ImGui::Text(BbSettings::MenuText("%.0f FPS  %.1f ms  %s", "%.0f FPS  %.1f мс  %s"),
                frame_ms_avg > 0.0f ? 1000.0f / frame_ms_avg : 0.0f, frame_ms_avg,
                s.upscaler == BbSettings::UpscalerFsr3     ? "FSR 3.1"
                : s.upscaler == BbSettings::UpscalerFsr4   ? "FSR 4"
                : s.upscaler == BbSettings::UpscalerFsr411 ? "FSR 4.1.1"
                : s.upscaler == BbSettings::UpscalerTaa    ? "TAA"
                : s.upscaler == BbSettings::UpscalerDlss   ? "DLSS"
                                                           : "");
    ImGui::End();
}

void TextPrompt() {
    std::string title, text;
    {
        std::scoped_lock lock{prompt_mutex};
        title = prompt_title;
        text = prompt_text;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.9f);
    ImGui::Begin("##textprompt", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextUnformatted(title.c_str());
    ImGui::Separator();
    ImGui::Text("%s_", text.c_str());
    ImGui::Separator();
    ImGui::TextUnformatted("Keyboard: type, Backspace = delete, Enter = OK, Esc = cancel");
    ImGui::End();
}

/// BB_MENU_KEYS_FILE=<file> (scripted tests): tokens toggle up down left right enter back l1 r1,
/// consumed when the file appears (it is removed), one key press per frame.
void ScriptedKeys() {
    static const char* path = std::getenv("BB_MENU_KEYS_FILE");
    static std::chrono::steady_clock::time_point last_check{};
    static std::vector<std::string> queue;
    static bool release = false;
    static ImGuiKey held = ImGuiKey_None;
    if (!path) {
        return;
    }
    ImGuiIO& io = ImGui::GetIO();
    if (release) {
        io.AddKeyEvent(held, false);
        release = false;
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (queue.empty() && now - last_check > std::chrono::milliseconds(100)) {
        last_check = now;
        if (FILE* f = std::fopen(path, "r")) {
            char token[32];
            while (std::fscanf(f, "%31s", token) == 1) {
                queue.emplace_back(token);
            }
            std::fclose(f);
            std::remove(path);
        }
    }
    if (queue.empty()) {
        return;
    }
    const std::string token = queue.front();
    queue.erase(queue.begin());
    if (token == "toggle") {
        SetOpen(!menu_open);
        return;
    }
    held = token == "up" ? ImGuiKey_GamepadDpadUp : token == "down" ? ImGuiKey_GamepadDpadDown
         : token == "left" ? ImGuiKey_GamepadDpadLeft : token == "right" ? ImGuiKey_GamepadDpadRight
         : token == "enter" ? ImGuiKey_GamepadFaceDown : token == "l1" ? ImGuiKey_GamepadL1
         : token == "r1" ? ImGuiKey_GamepadR1 : token == "kdown" ? ImGuiKey_DownArrow
         : token == "kup" ? ImGuiKey_UpArrow : ImGuiKey_None;
    if (token == "back") {
        SetOpen(false);
        return;
    }
    if (held != ImGuiKey_None) {
        io.AddKeyEvent(held, true);
        release = true;
    }
}

// The trophy chime: a short two-note bell made here (three notes for the platinum), on its
// own SDL stream beside the game's. BB_AUDIO=none keeps it silent like the game.
void Chime(bool platinum) {
    static SDL_AudioStream* stream = []() -> SDL_AudioStream* {
        const char* mode = std::getenv("BB_AUDIO");
        if ((mode && !std::strcmp(mode, "none")) || !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            return nullptr;
        }
        const SDL_AudioSpec spec{SDL_AUDIO_F32, 1, 48000};
        SDL_AudioStream* s =
            SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (s) {
            SDL_ResumeAudioStreamDevice(s);
        }
        return s;
    }();
    if (!stream) {
        return;
    }
    constexpr int rate = 48000;
    constexpr float two_pi = 6.2831853f;
    std::vector<float> pcm(rate * 6 / 5);
    const float notes[] = {1318.5f, 1975.5f, 2637.0f}; // E6, B6, E7
    for (int k = 0; k < (platinum ? 3 : 2); ++k) {
        for (std::size_t i = std::size_t(k * 0.09f * rate); i < pcm.size(); ++i) {
            const float t = float(i) / rate - k * 0.09f, f = notes[k];
            const float envelope = std::min(t / 0.004f, 1.0f) * std::exp(-t * 5.0f);
            // A bell: the fundamental and a quickly fading inharmonic partial.
            pcm[i] += 0.12f * envelope *
                      (std::sin(two_pi * f * t) + 0.35f * std::exp(-t * 8.0f) * std::sin(two_pi * 2.76f * f * t));
        }
    }
    SDL_PutAudioStreamData(stream, pcm.data(), int(pcm.size() * sizeof(float)));
}

// The PS4's trophy banner: a dark panel that slides in at the top left with the trophy cup
// in its grade's colour, "You have earned a trophy." and the trophy's name.
void Toast() {
    constexpr float seconds = 5.0f, slide = 0.4f;
    Banner toast;
    float age;
    bool play = false;
    {
        std::scoped_lock lock{toast_mutex};
        const auto now = std::chrono::steady_clock::now();
        age = std::chrono::duration<float>(now - toast_since).count();
        if (age > seconds) {
            toasts.pop_front();
            toast_since = now;
            age = 0.0f;
        }
        toast_pending = !toasts.empty();
        if (toasts.empty()) {
            return;
        }
        play = !toasts.front().sounded;
        toasts.front().sounded = true;
        toast = toasts.front();
    }
    if (play) {
        Chime(toast.grade == 4);
    }
    static const ImU32 grade_colors[] = {
        IM_COL32(200, 200, 200, 255), IM_COL32(205, 133, 77, 255), IM_COL32(199, 204, 214, 255),
        IM_COL32(245, 199, 66, 255), IM_COL32(158, 209, 255, 255)};
    const ImU32 cup = grade_colors[std::clamp(toast.grade, 0, 4)];
    // Slide in and out (ease-out cubic), fading with the motion.
    const float in = std::clamp(std::min(age, seconds - age) / slide, 0.0f, 1.0f);
    const float eased = 1.0f - (1.0f - in) * (1.0f - in) * (1.0f - in);

    const float s = base_scale;
    ImFont* font = ImGui::GetFont();
    const float small = ImGui::GetFontSize(), large = small * 1.2f;
    const char* line = BbSettings::MenuText("You have earned a trophy.", "Вы получили трофей.");
    const float text_w = std::max(font->CalcTextSizeA(large, FLT_MAX, 0.0f, toast.name.c_str()).x,
                                  font->CalcTextSizeA(small, FLT_MAX, 0.0f, line).x);
    const float height = 84.0f * s, icon = 60.0f * s, gap = 12.0f * s;
    const float width = std::max(360.0f * s, gap + icon + gap * 1.5f + text_w + gap * 2.0f);
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float margin = 28.0f * s;
    const ImVec2 at(viewport->WorkPos.x + margin - (width + margin) * (1.0f - eased),
                    viewport->WorkPos.y + margin);
    const auto alpha = [&](ImU32 c, float a) {
        return (c & ~IM_COL32_A_MASK) | (ImU32(((c >> IM_COL32_A_SHIFT) & 0xff) * a * eased) << IM_COL32_A_SHIFT);
    };

    ImDrawList* draw = ImGui::GetForegroundDrawList();
    const ImVec2 end(at.x + width, at.y + height);
    draw->AddRectFilled(ImVec2(at.x + 3 * s, at.y + 4 * s), ImVec2(end.x + 3 * s, end.y + 4 * s),
                        alpha(IM_COL32(0, 0, 0, 255), 0.35f), 10.0f * s); // shadow
    draw->AddRectFilled(at, end, alpha(IM_COL32(24, 26, 31, 255), 0.94f), 10.0f * s);
    draw->AddRect(at, end, alpha(IM_COL32(255, 255, 255, 255), 0.10f), 10.0f * s, 0, 1.0f * s);

    // The icon tile and the cup: bowl, handles, stem and base.
    const ImVec2 tile(at.x + gap, at.y + (height - icon) * 0.5f);
    draw->AddRectFilled(tile, ImVec2(tile.x + icon, tile.y + icon), alpha(IM_COL32(44, 48, 56, 255), 1.0f), 6.0f * s);
    const ImVec2 c(tile.x + icon * 0.5f, tile.y + icon * 0.5f);
    const float u = icon * 0.5f;
    const ImU32 cup_col = alpha(cup, 1.0f);
    draw->AddRectFilled(ImVec2(c.x - 0.42f * u, c.y - 0.55f * u), ImVec2(c.x + 0.42f * u, c.y + 0.05f * u), cup_col,
                        0.38f * u, ImDrawFlags_RoundCornersBottom);
    draw->AddCircle(ImVec2(c.x - 0.45f * u, c.y - 0.32f * u), 0.17f * u, cup_col, 0, 0.09f * u);
    draw->AddCircle(ImVec2(c.x + 0.45f * u, c.y - 0.32f * u), 0.17f * u, cup_col, 0, 0.09f * u);
    draw->AddRectFilled(ImVec2(c.x - 0.07f * u, c.y + 0.02f * u), ImVec2(c.x + 0.07f * u, c.y + 0.36f * u), cup_col);
    draw->AddRectFilled(ImVec2(c.x - 0.30f * u, c.y + 0.34f * u), ImVec2(c.x + 0.30f * u, c.y + 0.50f * u), cup_col,
                        0.05f * u);
    draw->AddRectFilled(ImVec2(c.x - 0.28f * u, c.y - 0.48f * u), ImVec2(c.x - 0.16f * u, c.y - 0.10f * u),
                        alpha(IM_COL32(255, 255, 255, 255), 0.35f), 0.06f * u); // shine

    const float x = tile.x + icon + gap * 1.5f;
    const float top = at.y + (height - small - large - 6.0f * s) * 0.5f;
    draw->AddText(font, small, ImVec2(x, top), alpha(IM_COL32(170, 176, 186, 255), 1.0f), line);
    draw->AddText(font, large, ImVec2(x, top + small + 6.0f * s), alpha(IM_COL32(255, 255, 255, 255), 1.0f),
                  toast.name.c_str());
}

// The trophy list, laid out like the PS4 one: progress, grade counts, earned/locked filter,
// hidden trophies masked until earned.
void TrophyList() {
    using BbSettings::MenuText;
    struct Row {
        const char *name, *description;
        int grade, hidden;
        int64_t when;
    };
    static constexpr int points[] = {0, 15, 30, 90, 180}; // PSN weights: progress counts points
    static const ImVec4 colors[] = {{}, {0.80f, 0.52f, 0.30f, 1.0f}, {0.78f, 0.80f, 0.84f, 1.0f},
                                    {0.96f, 0.78f, 0.26f, 1.0f}, {0.62f, 0.82f, 1.0f, 1.0f}};
    const char* grades[] = {"", MenuText("Bronze", "Бронза"), MenuText("Silver", "Серебро"),
                            MenuText("Gold", "Золото"), MenuText("Platinum", "Платина")};
    Row rows[64];
    int count = 0, earned = 0, got_points = 0, all_points = 0;
    int grade_earned[5]{}, grade_total[5]{};
    while (count < 64 && runtime_trophy(count, &rows[count].name, &rows[count].description,
                                        &rows[count].grade, &rows[count].hidden,
                                        &rows[count].when)) {
        const Row& r = rows[count++];
        const int g = std::clamp(r.grade, 0, 4);
        all_points += points[g];
        ++grade_total[g];
        if (r.when) {
            ++earned;
            got_points += points[g];
            ++grade_earned[g];
        }
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(760.0f * base_scale, 640.0f * base_scale), ImGuiCond_Appearing);
    if (!ImGui::Begin(MenuText("Trophies###bbport_trophies", "Трофеи###bbport_trophies"),
                      &trophies_open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    const float progress = all_points ? float(got_points) / float(all_points) : 0.0f;
    ImGui::Text(MenuText("Earned %d of %d", "Получено %d из %d"), earned, count);
    char percent[16];
    std::snprintf(percent, sizeof(percent), "%d%%", int(progress * 100.0f));
    ImGui::ProgressBar(progress, ImVec2(-1.0f, 0.0f), percent);
    for (int g = 4; g >= 1; --g) {
        ImGui::TextColored(colors[g], "%s %d/%d", grades[g], grade_earned[g], grade_total[g]);
        ImGui::SameLine(0.0f, 24.0f * base_scale);
    }
    ImGui::NewLine();

    static int filter = 0; // all, earned, not earned
    ImGui::RadioButton(MenuText("All", "Все"), &filter, 0);
    ImGui::SameLine();
    ImGui::RadioButton(MenuText("Earned", "Полученные"), &filter, 1);
    ImGui::SameLine();
    ImGui::RadioButton(MenuText("Not earned", "Не полученные"), &filter, 2);

    if (ImGui::BeginTable("##trophies", 3,
                          ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_BordersInnerH,
                          ImVec2(0.0f, -1.0f))) {
        ImGui::TableSetupColumn("##grade", ImGuiTableColumnFlags_WidthFixed, 90.0f * base_scale);
        ImGui::TableSetupColumn("##trophy", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("##date", ImGuiTableColumnFlags_WidthFixed, 150.0f * base_scale);
        int group_shown = -1;
        for (int i = 0; i < count; ++i) {
            const Row& r = rows[i];
            if ((filter == 1 && !r.when) || (filter == 2 && r.when)) {
                continue;
            }
            const int group = i >= 34 ? 1 : 0; // 34..39: The Old Hunters
            if (group != group_shown) {
                group_shown = group;
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(1);
                ImGui::SeparatorText(group ? "The Old Hunters" : "Bloodborne");
            }
            ImGui::TableNextRow();
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, r.when ? 1.0f : 0.55f);
            ImGui::TableSetColumnIndex(0);
            ImGui::TextColored(colors[std::clamp(r.grade, 0, 4)], "%s", grades[std::clamp(r.grade, 0, 4)]);
            ImGui::TableSetColumnIndex(1);
            if (r.hidden && !r.when) {
                ImGui::TextUnformatted(MenuText("Hidden Trophy", "Скрытый трофей"));
                ImGui::TextDisabled("%s", MenuText("Keep playing to reveal this trophy.",
                                                   "Продолжайте играть, чтобы открыть этот трофей."));
            } else {
                ImGui::TextUnformatted(r.name);
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextDisabled("%s", r.description);
                ImGui::PopTextWrapPos();
            }
            ImGui::TableSetColumnIndex(2);
            if (r.when > 1) {
                const std::time_t t = std::time_t(r.when);
                char date[32] = "";
                if (const std::tm* local = std::localtime(&t)) { // present thread only
                    std::strftime(date, sizeof(date), "%Y-%m-%d %H:%M", local);
                }
                ImGui::TextUnformatted(date);
            } else if (r.when) {
                ImGui::TextUnformatted(MenuText("Earned", "Получен"));
            } else {
                ImGui::TextDisabled("%s", MenuText("Not earned", "Не получен"));
            }
            ImGui::PopStyleVar();
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

} // namespace

void Notify(const std::string& name, int grade) {
    std::scoped_lock lock{toast_mutex};
    if (toasts.empty()) {
        toast_since = std::chrono::steady_clock::now();
    }
    toasts.push_back({name, grade});
    toast_pending = true;
}

void SetTextPrompt(bool active, const std::string& prompt, const std::string& text) {
    {
        std::scoped_lock lock{prompt_mutex};
        prompt_title = prompt;
        prompt_text = text;
    }
    prompt_active = active;
}

void Init(const Vulkan::Instance& instance, vk::Format format, u32 image_count) {
    std::scoped_lock lock{imgui_mutex};
    if (initialized) {
        return;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // window positions are not kept
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.BackendPlatformName = "bbport";

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.Colors[ImGuiCol_WindowBg].w = 0.92f;

    ImFontConfig font_config;
    font_config.FontDataOwnedByAtlas = false;
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(bb_font_ttf),
                                   int(bb_font_ttf_end - bb_font_ttf), 18.0f, &font_config);

    const vk::Instance vk_instance = instance.GetInstance();
    ImGui_ImplVulkan_LoadFunctions(
        instance.ApiVersion(),
        [](const char* name, void* user) {
            return VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr(
                *static_cast<const vk::Instance*>(user), name);
        },
        const_cast<vk::Instance*>(&vk_instance));

    const VkFormat color_format = static_cast<VkFormat>(format);
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = instance.ApiVersion();
    info.Instance = vk_instance;
    info.PhysicalDevice = instance.GetPhysicalDevice();
    info.Device = instance.GetDevice();
    info.QueueFamily = instance.GetGraphicsQueueFamilyIndex();
    info.Queue = instance.GetGraphicsQueue();
    info.DescriptorPoolSize = 16;
    info.MinImageCount = std::max(image_count, 2u);
    info.ImageCount = std::max(image_count, 2u);
    info.UseDynamicRendering = true;
    info.PipelineInfoMain.PipelineRenderingCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &color_format,
    };
    if (!ImGui_ImplVulkan_Init(&info)) {
        std::printf("Overlay: ImGui Vulkan backend init failed\n");
        ImGui::DestroyContext();
        return;
    }
    initialized = true;
    std::printf("Overlay: menu ready (Insert or L3+R3)\n");
}

void UpdateTextInput(SDL_Window* window) {
    bool want = false;
    {
        std::scoped_lock lock{imgui_mutex};
        want = initialized && menu_open && ImGui::GetIO().WantTextInput;
    }
    if (want != SDL_TextInputActive(window)) {
        if (want) {
            SDL_StartTextInput(window);
        } else {
            SDL_StopTextInput(window);
        }
    }
}

bool HandleEvent(const SDL_Event& event) {
    std::scoped_lock lock{imgui_mutex};
    if (!initialized) {
        return false;
    }
    ImGuiIO& io = ImGui::GetIO();
    const bool is_open = menu_open;
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        const bool down = event.type == SDL_EVENT_KEY_DOWN;
        if (down && !event.key.repeat &&
            (event.key.key == SDLK_INSERT || (is_open && event.key.key == SDLK_ESCAPE))) {
            SetOpen(event.key.key == SDLK_INSERT ? !is_open : false);
            return true;
        }
        if (!is_open) {
            return false;
        }
        io.AddKeyEvent(ImGuiMod_Ctrl, (event.key.mod & SDL_KMOD_CTRL) != 0);
        io.AddKeyEvent(ImGuiMod_Shift, (event.key.mod & SDL_KMOD_SHIFT) != 0);
        io.AddKeyEvent(ImGuiMod_Alt, (event.key.mod & SDL_KMOD_ALT) != 0);
        if (const ImGuiKey key = KeyFromSdl(event.key.key); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        return true;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        const bool down = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        const u8 button = event.gbutton.button;
        if (button == SDL_GAMEPAD_BUTTON_LEFT_STICK) {
            l3_down = down;
        } else if (button == SDL_GAMEPAD_BUTTON_RIGHT_STICK) {
            r3_down = down;
        }
        if (down && l3_down && r3_down) {
            SetOpen(!is_open);
            return true;
        }
        if (!is_open) {
            return false;
        }
        if (const ImGuiKey key = KeyFromGamepad(button); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        return true;
    }
    case SDL_EVENT_TEXT_INPUT: {
        // Typed characters (Ctrl+click on a slider, a text field): key events alone erase but
        // do not type. SDL sends them while text input is on (UpdateTextInput).
        if (!is_open) {
            return false;
        }
        io.AddInputCharactersUTF8(event.text.text);
        return true;
    }
    case SDL_EVENT_MOUSE_MOTION: {
        if (!is_open) {
            return false;
        }
        const float density = PixelDensity(event.motion.windowID);
        io.AddMousePosEvent(event.motion.x * density, event.motion.y * density);
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (!is_open) {
            return false;
        }
        const int button = event.button.button == SDL_BUTTON_LEFT    ? 0
                           : event.button.button == SDL_BUTTON_RIGHT  ? 1
                           : event.button.button == SDL_BUTTON_MIDDLE ? 2
                                                                      : -1;
        if (button >= 0) {
            io.AddMouseButtonEvent(button, event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        }
        return true;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        if (!is_open) {
            return false;
        }
        io.AddMouseWheelEvent(event.wheel.x, event.wheel.y);
        return true;
    default:
        return false;
    }
}

void SetBanner(const char* text) {
    {
        std::scoped_lock lock{banner_mutex};
        banner_text = text ? text : "";
    }
    banner_active = text != nullptr && text[0] != 0;
}

bool Visible() {
    return initialized && (menu_open || prompt_active || banner_active || toast_pending ||
                           BbSettings::Get().show_fps);
}

bool MenuOpen() {
    return menu_open;
}

bool CapturesInput() {
    // The text dialog too: keys typed into it (Backspace is the touchpad) stay out of the game.
    return menu_open || prompt_active;
}

void Render(vk::CommandBuffer cmdbuf, vk::ImageView view, vk::Extent2D extent) {

    // Present interval for the FPS readout (measured also while nothing is drawn).
    const auto now = std::chrono::steady_clock::now();
    const float ms = std::chrono::duration<float, std::milli>(now - last_present).count();
    last_present = now;
    if (ms > 0.0f && ms < 1000.0f) {
        frame_ms_avg = frame_ms_avg == 0.0f ? ms : frame_ms_avg * 0.95f + ms * 0.05f;
    }
    // BB_FPS_PRINT=1: the present rate every 5 s on stdout, without the cost of BB_FRAME_STATS
    // (its timers are in the draw path): to measure as players run the game.
    static const bool fps_print = [] {
        const char* env = std::getenv("BB_FPS_PRINT");
        return env && env[0] == '1';
    }();
    if (fps_print) {
        static auto window_start = now;
        static std::uint64_t presents = 0;
        ++presents;
        if (const double s = std::chrono::duration<double>(now - window_start).count(); s >= 5.0) {
            std::printf("Present rate: %.1f FPS over %.1f s\n", presents / s, s);
            std::fflush(stdout);
            window_start = now;
            presents = 0;
        }
    }
    if (std::getenv("BB_MENU_KEYS_FILE") && initialized) {
        std::scoped_lock lock{imgui_mutex};
        ScriptedKeys();
    }
    if (!Visible()) {
        return;
    }
    std::scoped_lock lock{imgui_mutex};
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(extent.width), float(extent.height));
    io.DeltaTime = ms > 0.0f && ms < 1000.0f ? ms / 1000.0f : 1.0f / 60.0f;
    // UI scale follows the display height (1080p = 1).
    const float scale = std::max(float(extent.height) / 1080.0f, 0.75f);
    if (std::abs(scale - base_scale) > 0.01f) {
        ImGuiStyle& style = ImGui::GetStyle();
        style.ScaleAllSizes(scale / base_scale);
        style.FontScaleMain = scale;
        base_scale = scale;
    }

    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    if (menu_open) {
        Menu();
        if (trophies_open) {
            TrophyList();
        }
    }
    if (BbSettings::Get().show_fps && !menu_open) {
        FpsCounter();
    }
    if (prompt_active && !menu_open) {
        TextPrompt();
    }
    if (banner_active) {
        std::string text;
        {
            std::scoped_lock banner_lock{banner_mutex};
            text = banner_text;
        }
        const ImVec2 size = ImGui::CalcTextSize(text.c_str());
        auto* draw = ImGui::GetForegroundDrawList();
        const ImVec2 at{(io.DisplaySize.x - size.x) * 0.5f, io.DisplaySize.y * 0.82f};
        draw->AddRectFilled({at.x - 24.0f, at.y - 12.0f}, {at.x + size.x + 24.0f, at.y + size.y + 12.0f},
                            IM_COL32(0, 0, 0, 170), 6.0f);
        draw->AddText(at, IM_COL32(230, 220, 200, 255), text.c_str());
    }
    if (toast_pending && !menu_open) {
        Toast();
    }
    ImGui::Render();

    const vk::RenderingAttachmentInfo attachment{
        .imageView = view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    cmdbuf.beginRendering(vk::RenderingInfo{
        .renderArea = {{0, 0}, extent},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &attachment,
    });
    {
        // Font atlas uploads submit to the graphics queue themselves.
        std::scoped_lock submit_lock{Vulkan::Scheduler::submit_mutex};
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmdbuf);
    }
    cmdbuf.endRendering();
}

} // namespace BbOverlay

extern "C" void bbgpu_overlay_banner(const char* text) {
    BbOverlay::SetBanner(text);
}
