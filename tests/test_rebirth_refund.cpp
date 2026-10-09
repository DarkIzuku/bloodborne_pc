// SPDX-License-Identifier: GPL-3.0-or-later
#include "gpu/shim/engine/rebirth_refund.h"
#include <cassert>
#include <limits>
#include <cstdio>
int main() {
    using namespace BbEngine::Rebirth;
    Build origin{10,{10,10,10,10,10,10},0},current{12,{12,10,10,10,10,10},500};
    float graph[4]={1,0,0,0};std::int64_t refund=-1;
    assert(Refund(current,origin,graph,&refund) && refund==91*91+92*92);
    current.level=11; // Edited level: refund actual attribute points, as bbhost does.
    assert(Refund(current,origin,graph,&refund) && refund==16745);
    current.echoes=EchoCap-1;assert(!Refund(current,origin,graph,&refund));current.echoes=500;
    current.stat[0]=999;assert(!Refund(current,origin,graph,&refund));current.stat[0]=12;
    graph[0]=std::numeric_limits<float>::quiet_NaN();assert(!Refund(current,origin,graph,&refund));
    graph[0]=std::numeric_limits<float>::max();assert(!Refund(current,origin,graph,&refund));graph[0]=1;
    current.level=1000000;assert(!Refund(current,origin,graph,&refund));
    current=origin;assert(Refund(current,origin,graph,&refund) && refund==0);
    std::puts("PASS: rebirth origin pricing, edited-level consistency, echo cap and invalid-save/graph rejection");
}
