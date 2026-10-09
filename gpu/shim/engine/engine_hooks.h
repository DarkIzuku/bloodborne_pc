// SPDX-License-Identifier: GPL-3.0-or-later
// Host bridge for the engine features adapted from bbhost; renderer-independent.
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace BbEngine {
using Callback = std::int64_t (__attribute__((sysv_abi)) *)(std::uint64_t, const std::uint64_t*);
struct Hook {
    std::uint8_t* site = nullptr;
    std::uint8_t* stub = nullptr;
    std::size_t bytes = 0;
};
// All hooks are prepared and checked before any entry is changed. Called at loader startup.
bool Prepare(Hook& hook, std::uint8_t* image, std::size_t size, std::size_t offset,
             std::span<const std::uint8_t> expected, Callback callback);
void Discard(Hook& hook);
void Commit(std::span<Hook> hooks);

// bloodborne_pc already rewrites guest TLS to the Win32 TEB; it does not need bbhost's
// FS-switching thunk. Clang bridges the guest SysV ABI and the host ABI at this call site.
template <typename T> std::int64_t Argument(T value) {
    if constexpr (std::is_pointer_v<T>) return reinterpret_cast<std::int64_t>(value);
    else return static_cast<std::int64_t>(value);
}
template <typename... A> std::int64_t Call(std::uint64_t fn, A... args) {
    static_assert(sizeof...(A) <= 6);
    using GuestFn = std::int64_t (__attribute__((sysv_abi)) *)(std::int64_t, std::int64_t,
        std::int64_t, std::int64_t, std::int64_t, std::int64_t);
    std::int64_t values[7] = {0, Argument(args)...};
    return reinterpret_cast<GuestFn>(fn)(values[1], values[2], values[3], values[4], values[5], values[6]);
}
void Install(std::uint8_t* image, std::size_t size);
} // namespace BbEngine
