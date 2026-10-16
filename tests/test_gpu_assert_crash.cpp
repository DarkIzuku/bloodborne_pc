// SPDX-License-Identifier: GPL-2.0-or-later
// Deliberately exercise the production GPU assertion path, outside the game.
#include <barrier>
#include <cstring>
#include <thread>
#include "../src/crash_win.h"
#include "common/assert.h"

int main(int argc, char** argv) {
    crash_win_init();
    if (argc>1 && !std::strcmp(argv[1], "secret")) {
        auto* key = static_cast<unsigned char*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE));
        if (!key) return 98;
        for (unsigned i=0; i<32; ++i) key[i]=static_cast<unsigned char>(0xd3u^(i*71u));
        if (!crash_win_protect_secret(key, 32)) return 97;
        // Keep the protected allocation reachable from this faulting stack.
        volatile auto* retained = key;
        (void)retained;
        ASSERT_MSG(false, "Intentional protected credential dump regression");
    }
    if (argc>1 && !std::strcmp(argv[1],"concurrent")) {
        std::barrier ready{3};
        auto fail=[&] {
            ready.arrive_and_wait();
            ASSERT_MSG(false, "Intentional concurrent GPU assertion for dump regression");
        };
        std::jthread first(fail), second(fail);
        ready.arrive_and_wait();
    } else if (argc>1 && !std::strcmp(argv[1],"worker")) {
        std::jthread worker([] {
            ASSERT_MSG(false, "Intentional worker GPU assertion for dump regression");
        });
    } else {
        ASSERT_MSG(false, "Intentional main GPU assertion for dump regression");
    }
    return 99;
}
