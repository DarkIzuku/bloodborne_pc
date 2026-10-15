// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// FidelityFX's Windows host sources assume MSVC's _countof is available.
// Provide an array-only equivalent for Clang/MinGW without editing the SDK.
#include <cstddef>
#if defined(_WIN32) && !defined(_countof)
template <typename T, std::size_t N>
constexpr std::size_t BbFfxArrayCount(const T (&)[N]) noexcept { return N; }
#define _countof(array) BbFfxArrayCount(array)
#endif
