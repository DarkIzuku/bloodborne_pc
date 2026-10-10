// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <vector>
#include <string>
#include <utility>
namespace BbInputConfig {
using Items=std::vector<std::pair<std::string,std::string>>;
// Registered before game threads start; saves run on the engine frame thread.
inline void (*append_bindings)(Items&)=nullptr;
inline void Append(Items& items) {if(append_bindings)append_bindings(items);}
}
