// SPDX-License-Identifier: GPL-2.0-or-later
// Standalone CPU regression checks; no Vulkan device required.
#include <cassert>
#include "gpu/shadps4/video_core/renderer_vulkan/pipeline_cache_blob.h"

int main() {
    using namespace Vulkan::DriverCache;
    Identity id{0x1002, 0x1234, 42, 3, {1, 2, 3, 4}};
    std::vector<uint8_t> payload(48, 0);
    Write32(payload, 0, 32);
    Write32(payload, 4, 1);
    Write32(payload, 8, id.vendor);
    Write32(payload, 12, id.device);
    std::copy(id.uuid.begin(), id.uuid.end(), payload.begin() + 16);
    auto file = Encode(payload, id);
    assert(Decode(file, id).size() == payload.size());
    for (size_t n = 0; n < file.size(); ++n) {
        assert(Decode(std::span(file).first(n), id).empty());
    }
    auto other = id;
    ++other.vendor; assert(Decode(file, other).empty());
    other = id; ++other.device; assert(Decode(file, other).empty());
    other = id; ++other.driver_version; assert(Decode(file, other).empty());
    other = id; ++other.driver_id; assert(Decode(file, other).empty());
    other = id; ++other.uuid[0]; assert(Decode(file, other).empty());
    // Every single-byte corruption, including the opaque payload, is rejected.
    for (size_t n = 0; n < file.size(); ++n) {
        file[n] ^= 1;
        assert(Decode(file, id).empty());
        file[n] ^= 1;
    }
    file.push_back(0); assert(Decode(file, id).empty());
    Write32(payload, 0, 16); assert(Encode(payload, id).empty());
    Write32(payload, 0, 32); Write32(payload, 4, 2);
    assert(Encode(payload, id).empty());
    Write32(payload, 4, 1); Write32(payload, 8, 0);
    assert(Encode(payload, id).empty());
    assert(Decode({}, id).empty());
}
