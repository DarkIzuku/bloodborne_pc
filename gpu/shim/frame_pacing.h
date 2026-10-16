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

// One owner, the present thread. Keep the slot phase across ordinary timer wake
// overshoot; reset it after a stall instead of submitting catch-up frames.
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
    void Presented(Time now) {
        if (!have_frame_ || !period_.count() || now < next_ ||
            now - next_ >= period_ / 2) {
            next_ = now + period_;
        } else {
            // Reusing now here accumulates every scheduler wake delay and can
            // reduce a 60 FPS limit to roughly 57 FPS on Windows. Retain the
            // deadline phase only while at least half a frame remains ahead.
            next_ += period_;
        }
        last_ = now;
        have_frame_ = true;
    }
private:
    std::chrono::nanoseconds period_{};
    Time next_{}, last_{};
    bool have_frame_ = false;
};
} // namespace BbFramePacing
