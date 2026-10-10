// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from bbhost src/core/sha256.h at 7c790536.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Hex SHA-256 of a buffer.
std::string sha256_hex(const std::uint8_t* data, std::size_t len);
