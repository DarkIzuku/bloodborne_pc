// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace Vulkan::DriverCache {
// A single bounded file per title. The envelope is independent of C++ struct packing;
// the payload remains the opaque driver blob, including its Vulkan header.
inline constexpr size_t MaxBytes = 256 * 1024 * 1024;
inline constexpr size_t HeaderBytes = 56;
inline constexpr uint32_t Magic = 0x43425042; // BPBC
inline constexpr uint32_t Version = 1;

struct Identity {
    uint32_t vendor, device, driver_version, driver_id;
    std::array<uint8_t, 16> uuid;
};

inline uint32_t Read32(std::span<const uint8_t> bytes, size_t offset) {
    return uint32_t(bytes[offset]) | uint32_t(bytes[offset + 1]) << 8 |
           uint32_t(bytes[offset + 2]) << 16 | uint32_t(bytes[offset + 3]) << 24;
}

inline void Write32(std::span<uint8_t> bytes, size_t offset, uint32_t value) {
    for (size_t i = 0; i < 4; ++i) {
        bytes[offset + i] = uint8_t(value >> (i * 8));
    }
}

inline uint64_t Checksum(std::span<const uint8_t> bytes) {
    uint64_t value = 14695981039346656037ULL;
    for (auto byte : bytes) {
        value = (value ^ byte) * 1099511628211ULL;
    }
    return value;
}

inline bool ValidPayload(std::span<const uint8_t> bytes, const Identity& id) {
    // VkPipelineCacheHeaderVersionOne has a specified 32-byte little-endian layout.
    return bytes.size() >= 32 && bytes.size() <= MaxBytes &&
           Read32(bytes, 0) == 32 && Read32(bytes, 4) == 1 &&
           Read32(bytes, 8) == id.vendor && Read32(bytes, 12) == id.device &&
           std::equal(id.uuid.begin(), id.uuid.end(), bytes.begin() + 16);
}

inline std::span<const uint8_t> Decode(std::span<const uint8_t> file, const Identity& id) {
    if (file.size() < HeaderBytes || file.size() > HeaderBytes + MaxBytes ||
        Read32(file, 0) != Magic || Read32(file, 4) != Version ||
        Read32(file, 8) != id.vendor || Read32(file, 12) != id.device ||
        Read32(file, 16) != id.driver_version || Read32(file, 20) != id.driver_id ||
        !std::equal(id.uuid.begin(), id.uuid.end(), file.begin() + 24) ||
        Read32(file, 40) != file.size() - HeaderBytes || Read32(file, 44) != sizeof(void*)) {
        return {};
    }
    const auto payload = file.subspan(HeaderBytes);
    const uint64_t checksum = Read32(file, 48) | uint64_t(Read32(file, 52)) << 32;
    return ValidPayload(payload, id) && Checksum(payload) == checksum ? payload
                                                                    : std::span<const uint8_t>{};
}

inline std::vector<uint8_t> Encode(std::span<const uint8_t> payload, const Identity& id) {
    if (!ValidPayload(payload, id)) {
        return {};
    }
    std::vector<uint8_t> file(HeaderBytes + payload.size());
    Write32(file, 0, Magic);
    Write32(file, 4, Version);
    Write32(file, 8, id.vendor);
    Write32(file, 12, id.device);
    Write32(file, 16, id.driver_version);
    Write32(file, 20, id.driver_id);
    std::copy(id.uuid.begin(), id.uuid.end(), file.begin() + 24);
    Write32(file, 40, uint32_t(payload.size()));
    Write32(file, 44, sizeof(void*));
    const auto checksum = Checksum(payload);
    Write32(file, 48, uint32_t(checksum));
    Write32(file, 52, uint32_t(checksum >> 32));
    std::copy(payload.begin(), payload.end(), file.begin() + HeaderBytes);
    return file;
}
} // namespace Vulkan::DriverCache
