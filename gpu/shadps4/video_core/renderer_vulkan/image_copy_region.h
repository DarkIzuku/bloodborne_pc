// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cstdint>
#include <limits>

namespace Vulkan {
struct CopyLayers {
    uint32_t source{}, destination{}, depth{};
    explicit operator bool() const { return source && destination && depth; }
};

// Counts belong to the selected subresources, not to the complete images. 3D images have
// one array layer; 2D/3D copies instead pair the 2D layer count with the volume copy depth.
inline CopyLayers PlanCopyLayers(bool source_3d, bool destination_3d,
                                 uint32_t source_layers, uint32_t destination_layers,
                                 uint32_t source_depth, uint32_t destination_depth,
                                 uint32_t source_base = 0, uint32_t destination_base = 0,
                                 uint32_t requested = std::numeric_limits<uint32_t>::max()) {
    if (!source_depth || !destination_depth || !requested ||
        source_base >= (source_3d ? 1u : source_layers) ||
        destination_base >= (destination_3d ? 1u : destination_layers)) {
        return {};
    }
    if (source_3d && destination_3d) {
        return {1, 1, std::min(source_depth, destination_depth)};
    }
    const auto available_source = source_3d ? source_depth : source_layers - source_base;
    const auto available_destination =
        destination_3d ? destination_depth : destination_layers - destination_base;
    const auto count = std::min({available_source, available_destination, requested});
    return {source_3d ? 1u : count, destination_3d ? 1u : count,
            source_3d || destination_3d ? count : 1u};
}
} // namespace Vulkan
