// SPDX-License-Identifier: GPL-3.0-or-later
// A BC7 encoder for the keyboard prompts (engine/button_prompts.cpp): mode 5 only, which keeps
// alpha apart from colour - a keycap's dark face, white letter and transparent corners do not lie
// on one colour line, but its colours alone (face to letter) nearly do.
#pragma once
#include <cstdint>
#include <cstring>
#include <utility>

namespace BbBc7 {

// The GPU's thin micro tiling of 4x4 blocks (tile mode 13, as the game's TPFs keep its UI):
// 8x8-block tiles in rows, the blocks of one tile in Morton order.
inline std::uint32_t TiledBlock(std::uint32_t bx, std::uint32_t by, std::uint32_t blocks_wide) {
    const std::uint32_t x = bx & 7, y = by & 7;
    const std::uint32_t morton = (x & 1) | (y & 1) << 1 | (x & 2) << 1 | (y & 2) << 2 | (x & 4) << 2 | (y & 4) << 3;
    return ((by >> 3) * (blocks_wide >> 3) + (bx >> 3)) * 64 + morton;
}

namespace detail {
constexpr int kWeights[4] = {0, 21, 43, 64};
inline int Lerp(int a, int b, int w) { return ((64 - w) * a + w * b + 32) >> 6; }
inline int Expand7(int c) { return c << 1 | c >> 6; }
struct Bits {
    std::uint8_t* out;
    int at = 0;
    void Put(std::uint32_t v, int n) {
        for (int i = 0; i < n; ++i, ++at) out[at >> 3] |= std::uint8_t(((v >> i) & 1) << (at & 7));
    }
};
} // namespace detail

/// One 4x4 block of straight-alpha RGBA8 (row by row) to 16 bytes of BC7 mode 5.
inline void EncodeBlock(const std::uint8_t rgba[64], std::uint8_t out[16]) {
    using namespace detail;
    // Colour ends: the two visible pixels furthest apart.
    int ends[2] = {-1, -1}, far = -1;
    for (int i = 0; i < 16; ++i) {
        if (rgba[i * 4 + 3] < 16) continue;
        for (int j = i; j < 16; ++j) {
            if (rgba[j * 4 + 3] < 16) continue;
            int d = 0;
            for (int c = 0; c < 3; ++c) d += (rgba[i * 4 + c] - rgba[j * 4 + c]) * (rgba[i * 4 + c] - rgba[j * 4 + c]);
            if (d > far) far = d, ends[0] = i, ends[1] = j;
        }
    }
    int color[2][3] = {}; // 7 bits
    for (int e = 0; e < 2 && ends[0] >= 0; ++e)
        for (int c = 0; c < 3; ++c) color[e][c] = (rgba[ends[e] * 4 + c] * 127 + 127) / 255;
    int alpha[2] = {255, 0};
    for (int i = 0; i < 16; ++i) {
        alpha[0] = rgba[i * 4 + 3] < alpha[0] ? rgba[i * 4 + 3] : alpha[0];
        alpha[1] = rgba[i * 4 + 3] > alpha[1] ? rgba[i * 4 + 3] : alpha[1];
    }
    int color_index[16], alpha_index[16];
    for (int i = 0; i < 16; ++i) {
        int best = 1 << 30, best_a = 1 << 30;
        for (int k = 0; k < 4; ++k) {
            int d = 0;
            for (int c = 0; c < 3; ++c) {
                const int v = Lerp(Expand7(color[0][c]), Expand7(color[1][c]), kWeights[k]) - rgba[i * 4 + c];
                d += v * v;
            }
            const int a = Lerp(alpha[0], alpha[1], kWeights[k]) - rgba[i * 4 + 3];
            if (d < best) best = d, color_index[i] = k;
            if (a * a < best_a) best_a = a * a, alpha_index[i] = k;
        }
    }
    // The first index of each set has one bit fewer: its high bit is implied zero.
    if (color_index[0] & 2) {
        for (int c = 0; c < 3; ++c) std::swap(color[0][c], color[1][c]);
        for (int& k : color_index) k = 3 - k;
    }
    if (alpha_index[0] & 2) {
        std::swap(alpha[0], alpha[1]);
        for (int& k : alpha_index) k = 3 - k;
    }
    std::memset(out, 0, 16);
    Bits bits{out};
    bits.Put(1u << 5, 6); // mode 5
    bits.Put(0, 2);       // no channel rotation
    for (int c = 0; c < 3; ++c) bits.Put(color[0][c], 7), bits.Put(color[1][c], 7);
    bits.Put(alpha[0], 8), bits.Put(alpha[1], 8);
    for (int i = 0; i < 16; ++i) bits.Put(color_index[i], i ? 2 : 1);
    for (int i = 0; i < 16; ++i) bits.Put(alpha_index[i], i ? 2 : 1);
}

} // namespace BbBc7
