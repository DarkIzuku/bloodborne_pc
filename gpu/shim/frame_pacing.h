// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace BbFramePacing {
// No menu-open heuristic: an inventory, shop or System page is still gameplay.
enum class Scene { FrontEnd, Loading, Gameplay };
void Initialize(const std::uint8_t* image, std::size_t size, const char* identity);
Scene CurrentScene();
void SetDisplayRefresh(double hz);
void SetVSync(bool enabled);
std::chrono::nanoseconds CurrentPeriod(std::uint32_t requested_fps);

inline std::chrono::nanoseconds Period(std::uint32_t requested_fps, Scene scene,
                                       bool vsync, double refresh_hz) {
    double limit = requested_fps;
    if (scene != Scene::Gameplay) limit = limit > 0 ? std::min(limit, 60.0) : 60.0;
    if (vsync) {
        if (!std::isfinite(refresh_hz) || refresh_hz < 1 || refresh_hz > 1000) refresh_hz = 60;
        limit = limit > 0 ? std::min(limit, refresh_hz) : refresh_hz;
    }
    return limit > 0 ? std::chrono::nanoseconds(static_cast<std::int64_t>(std::ceil(1e9 / limit)))
                     : std::chrono::nanoseconds::zero();
}

// One owner, the present thread. A missed slot never creates a burst of catch-up frames.
class Limiter {
public:
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;
    void Configure(std::chrono::nanoseconds period, Time now) {
        if (period == period_) return;
        period_ = period;
        next_ = have_frame_ && period.count() ? std::max(now, last_ + period) : now;
    }
    bool Due(Time now) const { return !period_.count() || now >= next_; }
    Time Next() const { return next_; }
    void Presented(Time now) { last_ = now; have_frame_ = true; next_ = now + period_; }
private:
    std::chrono::nanoseconds period_{};
    Time next_{}, last_{};
    bool have_frame_ = false;
};
} // namespace BbFramePacing
