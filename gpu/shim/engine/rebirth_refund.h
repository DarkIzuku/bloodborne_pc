// SPDX-License-Identifier: GPL-3.0-or-later
// The bbhost origin/refund formula, with bounds before any character mutation.
#pragma once
#include <cstdint>
#include <cmath>
namespace BbEngine::Rebirth {
struct Build { std::int64_t level=0,stat[6]{},echoes=0; };
constexpr std::int64_t EchoCap=999999999;
inline bool Refund(const Build& current,const Build& origin,const float graph[4],std::int64_t* refund) {
    if (current.level<4 || current.level>544 || origin.level<4 || origin.level>544 ||
        current.echoes<0 || current.echoes>EchoCap) return false;
    std::int64_t gained=0;
    for (unsigned i=0;i<6;++i) {
        if (current.stat[i]<1 || current.stat[i]>99 || origin.stat[i]<1 || origin.stat[i]>99) return false;
        if (current.stat[i]>origin.stat[i]) gained+=current.stat[i]-origin.stat[i];
    }
    for (unsigned i=0;i<4;++i) if (!std::isfinite(graph[i])) return false;
    if (origin.level+gained>544) return false;
    std::int64_t total=0;
    for (std::int64_t level=origin.level;level<origin.level+gained;++level) {
        const float x=static_cast<float>(level+81);
        float k=x-graph[3];if (k<0) k=0;
        k=k*graph[2]+graph[0];
        const float value=x*x*k+graph[1]+0.5f;
        if (!std::isfinite(value) || value<0 || value>static_cast<float>(EchoCap)) return false;
        const auto cost=static_cast<std::int64_t>(value);
        if (cost>EchoCap-total) return false;
        total+=cost;
    }
    if (total>EchoCap-current.echoes) return false;
    *refund=total;return true;
}
}
