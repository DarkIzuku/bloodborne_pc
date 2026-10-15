// SPDX-License-Identifier: GPL-3.0-or-later
#include "frame_pacing.h"
#include "bbport_platform.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>

namespace BbFramePacing {
namespace {
std::atomic<std::uintptr_t> image_base{0};
std::atomic<double> display_hz{60};
std::atomic<bool> fifo{false};
constexpr std::size_t kFrontEndTest = 0x1bb2c60;
constexpr std::size_t kMenuFlow = 0x5580e38;
constexpr std::size_t kWorldChrMan = 0x553e878;
constexpr char k109[] = "071df19c8880086d97182dbc057bc8cb37badaca57d9112683836b24a0444c0a";
// sub_1fb2c60: load the menu flow singleton, test +0xb11, return !started.
// Verified against the supported 1.09 ELF; these are NOT a menu visibility flag.
constexpr std::array<std::uint8_t, 25> kFrontEndBytes = {
    0x48,0x8b,0x05,0xd1,0xe1,0x9c,0x03,0x48,0x85,0xc0,0x74,0x0b,
    0x80,0xb8,0x11,0x0b,0x00,0x00,0x00,0x0f,0x94,0xc0,0xc3,0x31,0xc0};
template<class T> bool Read(std::uintptr_t address, T& value) {
    return address && BbPlatform::ReadProcessMemory(reinterpret_cast<const void*>(address), &value, sizeof(value));
}
bool Add(std::uintptr_t base, std::size_t offset, std::uintptr_t& out) {
    if (!base || base > std::numeric_limits<std::uintptr_t>::max() - offset) return false;
    out = base + offset; return true;
}
template<class T> bool Field(std::uintptr_t base, std::size_t offset, T& out) {
    std::uintptr_t address = 0;
    return Add(base, offset, address) && Read(address, out);
}
}
void Initialize(const std::uint8_t* image, std::size_t size, const char* identity) {
    image_base.store(0, std::memory_order_release);
    std::array<std::uint8_t, kFrontEndBytes.size()> bytes{};
    if (!image || !identity || std::strcmp(identity, k109) || size < kMenuFlow + 8 ||
        !BbPlatform::ReadProcessMemory(image + kFrontEndTest, bytes.data(), bytes.size()) || bytes != kFrontEndBytes) {
        std::puts("Frame pacing: unverified menu state; safe 60 FPS ceiling retained"); return;
    }
    image_base.store(reinterpret_cast<std::uintptr_t>(image), std::memory_order_release);
    std::puts("Frame pacing: verified 1.09 title/loading guard, 60 FPS; in-world menus keep gameplay limit");
}
Scene CurrentScene() {
    const auto base = image_base.load(std::memory_order_acquire);
    std::uintptr_t flow = 0, world = 0, player = 0;
    std::uint8_t started = 0;
    if (!base || !Field(base,kMenuFlow,flow) || !Field(flow,0xb11,started) || !started)
        return Scene::FrontEnd;
    std::uint32_t block = ~0u;
    if (!Field(base,kWorldChrMan,world) || !Field(world,0x60,player) ||
        !Field(player,0x3f8,block) || block == ~0u) return Scene::Loading;
    return Scene::Gameplay;
}
void SetDisplayRefresh(double hz) {
    if (!std::isfinite(hz) || hz < 1 || hz > 1000) hz = 60;
    const auto old = display_hz.exchange(hz, std::memory_order_relaxed);
    if (old != hz) std::printf("Frame pacing: current display %.3f Hz\n", hz);
}
void SetVSync(bool enabled) { fifo.store(enabled, std::memory_order_relaxed); }
std::chrono::nanoseconds CurrentPeriod(std::uint32_t requested_fps) {
    return Period(requested_fps, CurrentScene(), fifo.load(std::memory_order_relaxed),
                  display_hz.load(std::memory_order_relaxed));
}
} // namespace BbFramePacing
