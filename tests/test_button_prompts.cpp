// SPDX-License-Identifier: GPL-3.0-or-later
// The keyboard prompts' BC7 encoder (gpu/shim/engine/bc7.h): a keycap edge - transparent corner,
// light border, dark face, white letter - comes back close, decoded per the BC7 spec (mode 5).
#include "engine/bc7.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>

namespace {
std::uint32_t Get(const std::uint8_t* b, int& at, int n) {
    std::uint32_t v = 0;
    for (int i = 0; i < n; ++i, ++at) v |= std::uint32_t((b[at >> 3] >> (at & 7)) & 1) << i;
    return v;
}
void DecodeMode5(const std::uint8_t b[16], std::uint8_t rgba[64]) {
    int at = 0;
    assert(Get(b, at, 6) == 1u << 5);
    assert(Get(b, at, 2) == 0);
    int e[2][4];
    for (int c = 0; c < 3; ++c)
        for (int k = 0; k < 2; ++k) {
            const int v = int(Get(b, at, 7));
            e[k][c] = v << 1 | v >> 6;
        }
    e[0][3] = int(Get(b, at, 8)), e[1][3] = int(Get(b, at, 8));
    int ci[16], ai[16];
    for (int i = 0; i < 16; ++i) ci[i] = int(Get(b, at, i ? 2 : 1));
    for (int i = 0; i < 16; ++i) ai[i] = int(Get(b, at, i ? 2 : 1));
    assert(at == 128);
    const int w[4] = {0, 21, 43, 64};
    for (int i = 0; i < 16; ++i) {
        for (int c = 0; c < 3; ++c) rgba[i * 4 + c] = std::uint8_t(((64 - w[ci[i]]) * e[0][c] + w[ci[i]] * e[1][c] + 32) >> 6);
        rgba[i * 4 + 3] = std::uint8_t(((64 - w[ai[i]]) * e[0][3] + w[ai[i]] * e[1][3] + 32) >> 6);
    }
}
} // namespace

int main() {
    const std::uint8_t px[4][4] = {{0, 0, 0, 0}, {200, 200, 200, 255}, {22, 22, 24, 255}, {240, 240, 240, 255}};
    std::uint8_t block[64], encoded[16], decoded[64];
    for (int i = 0; i < 16; ++i)
        for (int c = 0; c < 4; ++c) block[i * 4 + c] = px[(i % 4 + i / 4) % 4][c];
    BbBc7::EncodeBlock(block, encoded);
    DecodeMode5(encoded, decoded);
    for (int i = 0; i < 16; ++i) {
        assert(std::abs(decoded[i * 4 + 3] - block[i * 4 + 3]) <= 2);
        if (block[i * 4 + 3])
            // Four levels from face to letter: a third colour lands within a sixth of that span.
            for (int c = 0; c < 3; ++c) assert(std::abs(decoded[i * 4 + c] - block[i * 4 + c]) <= (240 - 22) / 6 + 2);
    }
    // One 8x8-block tile holds a 32x32 glyph; a 1024-wide atlas has 32 tiles a row.
    assert(BbBc7::TiledBlock(1, 0, 8) == 1 && BbBc7::TiledBlock(0, 1, 8) == 2 && BbBc7::TiledBlock(7, 7, 8) == 63);
    assert(BbBc7::TiledBlock(8, 0, 256) == 64 && BbBc7::TiledBlock(0, 8, 256) == 32 * 64);
    for (const std::uint8_t b : encoded) std::printf("%02x", b);
    std::puts("\nbutton prompts: ok");
}
