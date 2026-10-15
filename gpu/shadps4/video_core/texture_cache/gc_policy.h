// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cstdint>
namespace VideoCore::GcPolicy {
// Never lower a driver's existing safety threshold; avoid multiplying a u64
// budget before dividing it. Zero/unknown budgets preserve the current policy.
constexpr std::uint64_t Pressure(std::uint64_t baseline, std::uint64_t budget) {
    return std::max(baseline, (budget / 100) * 80 + (budget % 100) * 80 / 100);
}
constexpr bool CompletedUse(std::uint64_t access, std::uint64_t recording,
                            bool gpu_complete) {
    return access < recording && gpu_complete;
}
// Compare by division before multiplying untrusted layout dimensions.
constexpr bool FullMip(std::uint32_t pitch, std::uint32_t height,
                       std::uint32_t layers, std::uint32_t bytes,
                       std::uint64_t size) {
    if (!pitch || !height || !layers || !bytes) return false;
    for (const auto factor : {pitch, height, layers, bytes}) {
        if (size % factor) return false;
        size /= factor;
    }
    return size == 1;
}
constexpr bool WritebackMipChain(bool macro_tiled, std::uint32_t levels) {
    // Macro mip tails may switch to micro tiling. The shared tiling dispatch
    // uses one macro specialization, so do not round-trip such a chain.
    return levels && levels <= 16 && (!macro_tiled || levels == 1);
}
}
