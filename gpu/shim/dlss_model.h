// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <string>
#include <string_view>

namespace BbSettings {
// NGX's official numeric values; G/H/I/N/O are reserved. A-D are legacy hints.
// NVIDIA/DLSS include/nvsdk_ngx_defs.h (310.x): J=10 K=11 L=12 M=13.
inline constexpr std::array DlssPresets{0, 1, 2, 3, 4, 5, 6, 10, 11, 13, 12};
inline constexpr const char* DlssPresetName(int value) {
    switch(value) {
    case 1: return "A"; case 2: return "B"; case 3: return "C"; case 4: return "D";
    case 5: return "E"; case 6: return "F"; case 10: return "J"; case 11: return "K";
    case 12: return "L"; case 13: return "M"; default: return "Auto";
    }
}
inline int ParseDlssPreset(std::string_view value) {
    auto first=value.find_first_not_of(" \t\r\n");
    if(first==value.npos) return 0;
    std::string text(value.substr(first, value.find_last_not_of(" \t\r\n")-first+1));
    for(char& c:text) if(c>='a' && c<='z') c-=32;
    if(text=="AUTO" || text=="DEFAULT" || text=="0") return 0;
    for(int preset:DlssPresets)
        if(text==DlssPresetName(preset) || text==std::to_string(preset)) return preset;
    return 0;
}
inline constexpr std::array DlssHintKeys{
    "DLSS.Hint.Render.Preset.DLAA", "DLSS.Hint.Render.Preset.Quality",
    "DLSS.Hint.Render.Preset.Balanced", "DLSS.Hint.Render.Preset.Performance",
    "DLSS.Hint.Render.Preset.UltraPerformance", "DLSS.Hint.Render.Preset.UltraQuality"};
}
