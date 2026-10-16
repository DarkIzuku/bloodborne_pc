// Local diagnostics for the original game session API.
#pragma once
#include <cstdint>
namespace Core::Bloodborne {
void TraceMatching2LeaveRoom(std::uintptr_t return_address, std::uint64_t room_id);
}
