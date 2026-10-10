// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
namespace BbEngine::MenuMemory {
bool Compatible(const std::uint8_t* image,std::size_t size);
bool Install(std::uint8_t* image,std::size_t size);
}
