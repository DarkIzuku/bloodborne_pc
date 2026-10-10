// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from bbhost engine/camera.cpp and params.cpp. Uses the real repository
// instead of scanning all guest memory: one authoritative LockCamParam table.
#include "camera.h"
#include "bbport_platform.h"
#include "bbport_settings.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace BbEngine::Camera {
namespace {
std::uint64_t slide;
bool active;
struct Row { std::uint64_t address; std::array<float,3> original, written; };
std::uint64_t table;
std::vector<Row> rows;
std::array<float,3> last_scale{1,1,1};
template<typename T> bool Read(std::uint64_t address, T* out) {
    return address && BbPlatform::ReadProcessMemory(reinterpret_cast<void*>(address), out, sizeof(T));
}
std::uint64_t Pointer(std::uint64_t address) { std::uint64_t value=0; Read(address,&value); return value; }
constexpr std::size_t fields[3] = {0x0,0xc,0x14}; // distance, height, vertical FOV
std::uint64_t FindTable() {
    const auto repository = Pointer(slide + 0x5540340); // BN 0x5940340 - preferred slide
    if (!repository) return 0;
    for (unsigned slot=0; slot<63; ++slot) {
        std::int32_t count=0;
        const auto entry=repository+0x70+slot*0x48;
        if (!Read(entry,&count) || count<=0) continue;
        for (unsigned k=0; k<static_cast<unsigned>(count) && k<8; ++k) {
            const auto cap=Pointer(entry+8+8*k);
            std::uint64_t capacity=0;
            if (!cap || !Read(cap+0x30,&capacity)) continue;
            const auto name=capacity<8 ? cap+0x18 : Pointer(cap+0x18);
            constexpr char16_t wanted[]=u"LockCamParam";
            char16_t found[sizeof wanted/sizeof(char16_t)]{};
            if (!name || !BbPlatform::ReadProcessMemory(reinterpret_cast<void*>(name),found,sizeof found) ||
                std::memcmp(found,wanted,sizeof wanted)) continue;
            const auto holder=Pointer(cap+0x70);
            return holder ? Pointer(holder+0x70) : 0;
        }
    }
    return 0;
}
bool LoadRows(std::uint64_t file) {
    char type[18]{};
    std::uint16_t count=0;
    if (!file || !Read(file+0xa,&count) || !count || count>4096 ||
        !BbPlatform::ReadProcessMemory(reinterpret_cast<void*>(file+0xc),type,sizeof type) ||
        std::strcmp(type,"LOCK_CAM_PARAM_ST")) return false;
    std::vector<Row> next;
    for (unsigned i=0;i<count;++i) {
        std::uint64_t offset=0;
        if (!Read(file+0x40+0x18*i+8,&offset) || offset<0x40+count*0x18u || offset>32*1024*1024) return false;
        Row row{}; row.address=file+offset;
        for (unsigned f=0;f<3;++f)
            if (!Read(row.address+fields[f],&row.original[f]) || !std::isfinite(row.original[f])) return false;
        if (!(row.original[2]>1 && row.original[2]<179)) return false;
        row.written=row.original;
        next.push_back(row);
    }
    rows=std::move(next); table=file;
    std::printf("Engine: camera LockCamParam found in the engine repository (%u rows)\n",count);
    return true;
}
}

bool Install(std::uint8_t* image, std::size_t size) {
    if (!BbSettings::Get().camera_controls) return false;
    constexpr std::size_t clamp=0x143af4e;
    constexpr std::uint8_t original[]={0x48,0x0f,0x46,0xc6}, lifted[]={0x48,0x89,0xf0,0x90};
    if (clamp>size || sizeof original>size-clamp ||
        (std::memcmp(image+clamp,original,sizeof original) && std::memcmp(image+clamp,lifted,sizeof lifted))) {
        std::puts("Engine: camera adjustments disabled: FOV clamp differs from 1.09"); return false;
    }
    std::memcpy(image+clamp,lifted,sizeof lifted);
    slide=reinterpret_cast<std::uint64_t>(image);
    active=true;
    std::puts("Engine: bbhost camera adjustments enabled (distance, height and FOV)");
    return true;
}

void Tick() {
    if (!active) return;
    const auto& settings=BbSettings::Get();
    const std::array<float,3> scale={settings.camera_distance_scale.load(),settings.camera_height_scale.load(),settings.camera_fov_scale.load()};
    if (scale==std::array<float,3>{1,1,1} && last_scale==scale) return;
    static unsigned ticks;
    if (!table || ticks++%150==0) {
        const auto found=FindTable();
        if (!found) return;
        if (found!=table && !LoadRows(found)) return;
    }
    char type[18]{};
    if (!BbPlatform::ReadProcessMemory(reinterpret_cast<void*>(table+0xc),type,sizeof type) ||
        std::strcmp(type,"LOCK_CAM_PARAM_ST")) { table=0; rows.clear(); return; }
    for (Row& row:rows) {
        for (unsigned f=0;f<3;++f) {
            float current=0;
            if (!Read(row.address+fields[f],&current) || !std::isfinite(current)) { table=0;rows.clear();return; }
            // Preserve bbhost's reload/external edit semantics; never multiply our own output.
            if (current!=row.written[f]) row.original[f]=current;
            const float wanted=row.original[f]*scale[f];
            if (current!=wanted) std::memcpy(reinterpret_cast<void*>(row.address+fields[f]),&wanted,sizeof wanted);
            row.written[f]=wanted;
        }
    }
    if (scale!=last_scale) {
        std::printf("Engine: camera distance x%.2f, height x%.2f, FOV x%.2f\n",scale[0],scale[1],scale[2]);
        last_scale=scale;
    }
}
} // namespace BbEngine::Camera
