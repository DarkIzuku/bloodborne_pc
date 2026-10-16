// SPDX-License-Identifier: GPL-2.0-or-later
// Keep test assertions enabled without changing the production Vulkan-Hpp ABI.
#pragma once
#include <cstdio>
#include <cstdlib>
[[noreturn]] inline void bb_test_assert_fail(const char* expression, const char* file, int line) {
    std::fprintf(stderr, "Assertion failed: %s (%s:%d)\n", expression, file, line);
    std::exit(1);
}
#undef assert
#define assert(expression) ((expression) ? (void)0 : bb_test_assert_fail(#expression, __FILE__, __LINE__))
