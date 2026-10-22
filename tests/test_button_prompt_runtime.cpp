// SPDX-License-Identifier: GPL-3.0-or-later
// Runs the real procedural glyph implementation without a window, Vulkan or game data.
#include <cassert>
#include <fstream>
#include <iterator>
#include <thread>
#include "../gpu/shim/engine/button_prompts.cpp"
void host_log(const char*, ...) {}
bool debug_menu_active() { return false; }
std::uint32_t menu_confirm_button() { return 0x4000; }
std::uint32_t menu_back_button() { return 0x2000; }

namespace BbOverlay {
const unsigned char* FontData() {
    static const std::vector<unsigned char> font = [] {
        std::ifstream file(BB_TEST_FONT_PATH, std::ios::binary);
        return std::vector<unsigned char>(std::istreambuf_iterator<char>(file), {});
    }();
    assert(!font.empty());
    return font.data();
}
}
namespace BbInput { bool Enabled() { return true; } }
namespace Core {
void MemoryManager::InvalidateMemory(VAddr, u64) {}
void MemoryManager::CopySparseMemory(VAddr source, u8* out, u64 size) {
    std::memcpy(out, reinterpret_cast<const void*>(source), size);
}
}
int main() {
    host_bindings_load_defaults();
    BbPrompts::Build(BbPrompts::kKeyboard);
    assert(BbPrompts::g_font_ready);
    for (const auto& glyph : BbPrompts::g_glyphs) assert(glyph.size() == 1024);
    for (const auto& spot : BbPrompts::g_spots) assert(spot.size() == 28 * 32 * 4);
    auto keyboard = BbPrompts::g_glyphs[15];
    // Device changes really regenerate textures, rather than only changing a log string.
    BbPrompts::Build(BbPrompts::kXbox);
    assert(keyboard != BbPrompts::g_glyphs[15]);
    BbPrompts::Build(BbPrompts::kKeyboard);
    assert(keyboard == BbPrompts::g_glyphs[15]);
    std::vector<std::uint8_t> foreign(1024, 0x5a);
    auto original = foreign;
    assert(!BbPrompts::Replace(reinterpret_cast<std::uint64_t>(foreign.data()), foreign.size()));
    assert(foreign == original); // unfamiliar/modded textures are preserved
    assert(!BbPrompts::Replace(0, 123)); // ignored sizes never dereference the address
    std::vector<std::thread> workers;
    for (int n=0; n<4; ++n) workers.emplace_back([n] {
        for (int i=0; i<8; ++i) {
            std::lock_guard lock{BbPrompts::g_mutex};
            BbPrompts::Build((i+n)&1 ? BbPrompts::kKeyboard : BbPrompts::kXbox);
            for (const auto& glyph : BbPrompts::g_glyphs) assert(glyph.size() == 1024);
        }
    });
    for (auto& worker : workers) worker.join();
    std::puts("PASS: real keyboard/Xbox generation, device switching, foreign texture preservation, concurrent ownership");
}
