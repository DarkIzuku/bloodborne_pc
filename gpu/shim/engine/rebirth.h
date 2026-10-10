// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
namespace BbEngine::Rebirth {
bool Install(std::uint8_t*,std::size_t);
void Tick();
}
