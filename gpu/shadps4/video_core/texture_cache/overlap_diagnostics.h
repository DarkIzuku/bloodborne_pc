// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdlib>
#include <string_view>

namespace VideoCore {
inline bool ImageOverlapLogging() {
    static const bool enabled = [] {
        const char* value = std::getenv("BB_IMAGE_OVERLAP_LOG");
        return value && std::string_view(value) == "1";
    }();
    return enabled;
}
} // namespace VideoCore
