// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
namespace BbEngine::Options {
bool Install(std::uint8_t* image,std::size_t size);
void Tick();
bool ClickDecides(std::uint8_t*,int);
void ListUpdate(std::uint8_t*,bool);
}
