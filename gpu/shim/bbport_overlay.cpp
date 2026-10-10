// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_overlay.h"

#include <algorithm>
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

#include <SDL3/SDL.h>
#include "bbport_settings.h"
#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

// DejaVu Sans (Cyrillic), embedded (third_party/fonts, Bitstream Vera license).
#ifdef _WIN32
// PE/COFF assemblers have no .hidden/.previous: the compiler embeds the file (#embed, a GCC
// extension in C++).
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
alignas(16) static const unsigned char bb_font_ttf[] = {
#embed BB_FONT_PATH
};
#pragma GCC diagnostic pop
static const unsigned char* const bb_font_ttf_end = bb_font_ttf + sizeof(bb_font_ttf);
#else
asm(".section .rodata\n"
    ".balign 16\n"
    ".hidden bb_font_ttf\n"
    ".global bb_font_ttf\n"
    "bb_font_ttf:\n"
    ".incbin \"" BB_FONT_PATH "\"\n"
    ".hidden bb_font_ttf_end\n"
    ".global bb_font_ttf_end\n"
    "bb_font_ttf_end:\n"
    ".previous\n");
extern "C" const unsigned char bb_font_ttf[];
extern "C" const unsigned char bb_font_ttf_end[];
#endif

extern "C" void runtime_restart(void); // bb-probe (probe.c)
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

// The widget runs before Store reads v: argument evaluation order is unspecified (clang on
// Windows copied v before the widget changed it, so clicks stored the old value).
void Checkbox(const char* label, std::atomic<bool>& value) {
    bool v = value;
    const bool changed = ImGui::Checkbox(label, &v);
    Store(value, v, changed);
}

void Slider(const char* label, std::atomic<float>& value, float lo, float hi) {
    float v = value;
    const bool changed = ImGui::SliderFloat(label, &v, lo, hi, "%.2f");
    Store(value, v, changed);
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
        BbSettings::Save();
    }
    ImGui::Text(BbSettings::MenuText("%.0f FPS  (%.1f ms)", "%.0f FPS  (%.1f мс)"),
                frame_ms_avg > 0.0f ? 1000.0f / frame_ms_avg : 0.0f, frame_ms_avg);

    ImGui::SeparatorText(BbSettings::MenuText("Temporal upscaler", "Временной апскейлер"));
    const char* upscalers[] = {
        BbSettings::MenuText("Off", "Выкл"), "FSR 3.1", "FSR 4 (INT8)", "FSR 4.1.1 (INT8)",
        BbSettings::MenuText("TAA (native anti-aliasing)", "TAA (нативное сглаживание)"), "DLSS (NVIDIA)"};
    static const char* later[] = {"XeSS"};
    int upscaler = s.upscaler;
    if (ImGui::BeginCombo(BbSettings::MenuText("Upscaler", "Апскейлер"), upscalers[upscaler])) {
        for (int i = 0; i < BbSettings::UpscalerCount; ++i) {
            const bool supported = i == BbSettings::UpscalerFsr4     ? s.fsr4_supported.load()
                                   : i == BbSettings::UpscalerFsr411 ? s.fsr411_supported.load()
                                                                     : i == BbSettings::UpscalerDlss ? s.dlss_supported.load() : true;
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
    if (const char* problem = s.fsr4_problem.load()) {
        ImGui::PushTextWrapPos();
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "Upscaler unavailable: %s", problem);
        if (!BbSettings::IsFsr4(s.upscaler))
            ImGui::TextUnformatted("The mode selected above is active. You can select it again to retry.");
        ImGui::PopTextWrapPos();
    }
    if (BbSettings::IsFsr4(s.upscaler)) {
        if (s.upscaler == BbSettings::UpscalerFsr411) {
            Hint(BbSettings::MenuText(
                "FSR 4.1.1 in INT8 mode: the model from AMD's 4.1.1 DLL, reproduced in Vulkan "
                "(output matches the DLL). One model for Native through Performance and another "
                "for Ultra Performance. Assets: tools/fsr4cap/build_assets.sh (requires the DLL "
                "and Proton).",
                "FSR 4.1.1 в режиме INT8: модель из DLL AMD 4.1.1, воспроизведённая в Vulkan "
                "(результат совпадает с DLL). Одна модель для Native..Performance и отдельная "
                "для Ultra Performance. Ассеты: tools/fsr4cap/build_assets.sh (нужны DLL и "
                "Proton)."));
        } else {
            Hint(BbSettings::MenuText(
                "FSR 4 in INT8 mode (v07 model from AMD FidelityFX SDK sources). Higher quality "
                "than FSR 3.1, but the pass is more demanding. Changing the preset rebuilds the "
                "model (a brief pause). Assets: tools/fetch_fsr4_assets.sh.",
                "FSR 4 в режиме INT8 (модель v07 из исходников AMD FidelityFX SDK). Качество выше, "
                "чем у FSR 3.1, но проход тяжелее. Смена пресета пересобирает модель (короткая "
                "пауза). Ассеты: tools/fetch_fsr4_assets.sh."));
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
            const int output = s.output_res;
            std::snprintf(
                label, sizeof(label),
                BbSettings::MenuText("%s (x%.1f, render %dx%d)", "%s (x%.1f, рендер %dx%d)"),
                BbSettings::PresetName(i), scale,
                int(std::lround(BbSettings::OutputWidths[output] / scale / 2) * 2),
                int(std::lround(BbSettings::OutputHeights[output] / scale / 2) * 2));
            if (ImGui::Selectable(label, i == preset)) {
                Store(s.preset, i, true);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    if (taa) {
        ImGui::TextWrapped("TAA anti-aliases the scene at the output resolution, without an FSR "
                           "model or upscaling. The saved preset returns when an upscaler is selected.");
    }
    ImGui::Text("Active scene render: %d x %d", s.active_render_width.load(),
                s.active_render_height.load());
    if (BbSettings::FixedRenderSession()) {
        ImGui::Text("Preset at startup: %s", BbSettings::PresetName(s.startup_preset));
        if (const char* automatic = std::getenv("BB_AUTO_RENDER_RES");
            automatic && automatic[0] == '1') {
            Hint("With an output other than 1080p the whole game renders at the preset resolution "
                 "(patched at startup): fastest on the Steam Deck and weaker GPUs. Preset or output "
                 "changes apply after a restart. \"Live resolution changes\" below allows changing "
                 "without a restart (post-processing then stays at 1080p, slower).");
        } else {
            Hint("BB_RENDER_RES fixes the scene size at startup. Remove this explicit variable "
                 "to change resolution and presets without restarting the game.");
        }
    } else {
        Hint("Native AA: the upscaler works as anti-aliasing. The other presets lower the scene "
             "render resolution relative to the output. The UI is drawn at the output resolution. "
             "The preset applies from the next frame without restarting the game.");
    }
    Checkbox("Sharpening (RCAS)", s.sharpen);
    ImGui::BeginDisabled(!s.sharpen);
    Slider("Sharpness", s.sharpness, 0.0f, 2.0f);
    Hint("Up to 1: the upscaler's own sharpening (RCAS). Above 1 another RCAS pass is added. "
         "DLSS has no sharpening of its own: an RCAS pass does all of it. "
         "Ctrl+click the slider to type an exact value.");
    ImGui::EndDisabled();
    Checkbox("Sub-pixel jitter", s.jitter);
    Hint("Each frame the scene shifts by a fraction of a pixel, and the upscaler gathers more "
         "detail from several frames. Without it you only get history-based anti-aliasing.");

    ImGui::SeparatorText("Reactive mask");
    ImGui::BeginDisabled(taa);
    Checkbox("Enable mask", s.reactive);
    Hint("Marks transparent effects (particles, haze) so the upscaler relies less on past "
         "frames. Fewer trails behind effects, but shimmer returns underneath them.");
    ImGui::BeginDisabled(!s.reactive);
    Slider("Scale", s.reactive_scale, 0.0f, 4.0f);
    Slider("Threshold", s.reactive_threshold, 0.0f, 1.0f);
    Slider("Maximum", s.reactive_max, 0.0f, 1.0f);
    bool show_mask = s.debug_view == BbSettings::DebugReactive;
    if (ImGui::Checkbox("Show mask (debug)", &show_mask)) {
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

    ImGui::SeparatorText(BbSettings::MenuText("Output resolution", "Разрешение вывода"));
    static const char* outputs[] = {"1280 x 720", "1920 x 1080", "2560 x 1440", "3840 x 2160",
        "2560 x 1080", "3440 x 1440", "3840 x 1600", "3840 x 1080", "5120 x 1440", "5120 x 2160"};
    static_assert(std::size(outputs)==BbSettings::OutputCount);
    int output = s.output_res;
    if (ImGui::BeginCombo(BbSettings::MenuText("Output resolution", "Разрешение вывода"),
                          outputs[output])) {
        for (int i = 0; i < BbSettings::OutputCount; ++i) {
            if (ImGui::Selectable(outputs[i], i == output)) {
                Store(s.output_res, i, true);
            }
        }
        ImGui::EndCombo();
    }
    if (BbSettings::FixedRenderSession()) {
        Hint(BbSettings::MenuText(
            "Size of the final frame and UI. The preset sets the scene size relative to "
            "the output: 4K Performance = 1920x1080. Applies after restarting the game.",
            "Размер готового кадра и интерфейса. Пресет задаёт размер сцены относительно "
            "вывода: 4K Performance = 1920x1080. Применяется после перезапуска игры."));
    } else {
        Hint(BbSettings::MenuText(
            "The final frame and UI size changes at the next frame boundary. "
            "The preset sets the scene size relative to the output: 4K Performance = 1920x1080. "
            "Changing the size resets FSR history and may cause a brief pause.",
            "Размер готового кадра и интерфейса меняется на границе следующего кадра. "
            "Пресет задаёт размер сцены относительно вывода: 4K Performance = 1920x1080. "
            "Смена размера сбрасывает историю FSR и может вызвать короткую паузу."));
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
    Hint(BbSettings::MenuText(
        "On: output resolution and preset change without restarting, but game post-processing "
        "stays at 1080p, which is noticeably slower on Steam Deck and older GPUs. "
        "Off: everything renders at the preset resolution, and changes require a restart. Auto "
        "enables this on powerful discrete GPUs. Applies after restarting the game.",
        "Включена: разрешение вывода и пресет меняются без перезапуска, но постобработка игры "
        "остаётся в 1080p — на Steam Deck и старых видеокартах это заметно медленнее. "
        "Выключена: всё рисуется в разрешении пресета, смена — через перезапуск. Авто включает "
        "её на мощных дискретных видеокартах. Применяется после перезапуска игры."));
    ImGui::SeparatorText(BbSettings::MenuText("Game effects (restart required)",
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
    }
    Hint(BbSettings::MenuText(
        "Effects are enabled and disabled by game patches at startup (patches/Bloodborne.xml). "
        "Motion blur and shadows from dynamic lights place a significant load on the GPU.",
        "Эффекты включаются и выключаются патчами игры при запуске (patches/Bloodborne.xml). "
        "Размытие в движении и тени от динамических источников заметно нагружают GPU."));
    Hint(BbSettings::MenuText(
        "Free camera: hold Cross and press L3 (keyboard: Space + Z). "
        "Debug menu: left touchpad / Tab. Requires DbgFont14h.ccm and DbgFont14h.tpf "
        "in dvdroot_ps4/font from Nexus mod #253. Right touchpad: Backspace.",
        "Свободная камера: удерживайте Cross и нажимайте L3 (клавиатура: Space + Z). "
        "Debug menu: левый touchpad / Tab. Нужны DbgFont14h.ccm и DbgFont14h.tpf "
        "в dvdroot_ps4/font из мода Nexus #253. Правый touchpad: Backspace."));

    bool restart =
        s.object_motion != s.startup_object_motion || s.model_lod != s.startup_model_lod ||
        s.live_resolution != s.startup_live_resolution || BbSettings::ResolutionNeedsRestart();
    for (int e = 0; e < BbSettings::EffectCount; ++e) {
        restart |= s.effects[e] != s.startup_effects[e];
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

    ImGui::SeparatorText("PC Camera");
    ImGui::TextDisabled("Enable PC Camera in the launcher before starting; changes here are live.");
    Slider("Camera FOV scale", s.camera_fov_scale, 1.0f, 1.5f);
    Slider("Camera distance scale", s.camera_distance_scale, 0.5f, 1.5f);
    Slider("Camera height scale", s.camera_height_scale, 0.5f, 1.5f);

    ImGui::SeparatorText(BbSettings::MenuText("Other", "Прочее"));
    Checkbox(BbSettings::MenuText("FPS counter in corner", "Счётчик FPS в углу"), s.show_fps);

    ImGui::Spacing();
    if (ImGui::Button(BbSettings::MenuText("Close", "Закрыть"))) {
        keep_open = false;
    }
    ImGui::SameLine();
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
    ImGui::Text("%.0f FPS  %.1f ms  %s", frame_ms_avg > 0.0f ? 1000.0f / frame_ms_avg : 0.0f,
                frame_ms_avg, BbSettings::UpscalerName(s.upscaler));
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

bool Visible() {
    return initialized &&
           (menu_open || prompt_active || toast_pending || BbSettings::Get().show_fps);
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
