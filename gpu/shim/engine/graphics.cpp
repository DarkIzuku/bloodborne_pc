// SPDX-License-Identifier: GPL-3.0-or-later
// Engine-side live settings adapted from bbhost graphics_patch.cpp at 7c790536.
// Retains bloodborne_pc's shaders, upscalers, resource lifetime and composition.
#include "graphics.h"
#include "engine_state.h"
#include "bbport_settings.h"
#include <atomic>
#include <cstring>
#include <cmath>
namespace BbEngine::Graphics {
namespace {
#define GUEST_ABI __attribute__((sysv_abi))
bool enabled;
constexpr std::uint64_t kRenderView = 0x269e990;
// push rbp; mov rbp, rsp; push r15..r12; push rbx; sub rsp, 0xea8
const std::uint8_t kRenderViewPrologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
                                            0x41, 0x54, 0x53, 0x48, 0x81, 0xec, 0xa8, 0x0e, 0x00, 0x00};
constexpr std::size_t kCapabilities = 0x48, kSsaoFlag = 0x2968, kAaFlag = 0x296a;
constexpr std::uint64_t kSsaoCapability = 0x8, kAaCapability = 0x10;

constexpr std::uint64_t kYebisRecord = 0x25d4860;
// push rbp; mov rbp, rsp; push r15..r12; push rbx; sub rsp, 0x28; mov r13, r9
const std::uint8_t kYebisRecordPrologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
                                             0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x28, 0x4d, 0x89, 0xcd};
constexpr std::uint64_t kPostBlock = 0x59406e0;
constexpr std::size_t kSaturation = 0xc8, kVignettePower = 0xd0, kGlareLuminance = 0xdc;
constexpr std::size_t kYebisHeld = 0x2710;  // on the YEBIS object: the block was not refreshed
std::uint8_t* g_post_block = nullptr;

// Ambient occlusion strength. The SSAO bank (+0x5968) is blended the same way
// (sub_1285500, into a 0x6c-byte block) and copied into the SSAO object the
// renderer keeps at +0x10d0 - to its +0x14c, unless its +0x148 byte holds it -
// right before sub_12669d0 draws the pass with it. Every area uses the second
// version of the pass (UseNewVersion 1), whose output is the visibility times
// "Ver2 AO Scale" (+0x44, 1.0 everywhere) plus "Ver2 AO Offset" (+0x48, 0):
// scale 0 left the clinic black (mean 15.4 -> 3.1) and 2 brightened it. So
// strength k makes the output 1 - k(1 - out) - the scale times k, the offset
// times k plus 1 - k: 0 is no occlusion, 1 the area's, 2 twice as dark.
constexpr std::uint64_t kSsaoDraw = 0x12669d0;
// push rbp; mov rbp, rsp; push r15..r12; push rbx; sub rsp, 0x2a8
const std::uint8_t kSsaoDrawPrologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
                                          0x41, 0x54, 0x53, 0x48, 0x81, 0xec, 0xa8, 0x02, 0x00, 0x00};
constexpr std::size_t kSsaoHeld = 0x148, kSsaoBlock = 0x14c;  // on the SSAO object
constexpr std::size_t kSsaoScale = 0x44, kSsaoOffset = 0x48;  // in its block

constexpr std::uint64_t kShadowSetup = 0x12ac210;
// push rbp; mov rbp, rsp; push r15; push r14; push rbx; sub rsp, 0x18;
// mov r14, rsi; mov rbx, rdi
const std::uint8_t kShadowSetupPrologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x48,
                                             0x83, 0xec, 0x18, 0x49, 0x89, 0xf6, 0x48, 0x89, 0xfb};
constexpr std::size_t kShadowBlock = 0x80, kShadowHeld = 0x297c;  // on the renderer
constexpr std::size_t kShadowScaled[] = {0x38, 0x3c, 0x44, 0x48};

template<typename T> bool Read(std::uint64_t at,T* out) { return at && host_read_safe(reinterpret_cast<void*>(at),out,sizeof(T)); }
template<typename T> void Write(std::uint64_t at,const T& value) { std::memcpy(reinterpret_cast<void*>(at),&value,sizeof(T)); }
GUEST_ABI std::int64_t RenderView(std::uint64_t,const std::uint64_t* saved) {
    const auto r=saved[0];std::uint64_t caps=0;
    if (!r || !Read(r+kCapabilities,&caps)) return 0;
    const std::uint8_t ssao=BbSettings::Get().effects[3].load() && (caps&kSsaoCapability);
    const std::uint8_t aa=BbSettings::Get().effects[4].load() && (caps&kAaCapability);
    Write(r+kSsaoFlag,ssao);Write(r+kAaFlag,aa);
    return 0;
}
GUEST_ABI std::int64_t AoDraw(std::uint64_t,const std::uint64_t* saved) {
    const auto object=saved[5];const float k=BbSettings::Get().graphics_ao_strength;
    std::uint8_t held=1;float v=0,o=0;
    if (!object || k==1 || !Read(object+kSsaoHeld,&held) || held ||
        !Read(object+kSsaoBlock+kSsaoScale,&v) || !Read(object+kSsaoBlock+kSsaoOffset,&o) || !std::isfinite(v) || !std::isfinite(o)) return 0;
    Write(object+kSsaoBlock+kSsaoScale,v*k);Write(object+kSsaoBlock+kSsaoOffset,o*k+(1-k));
    return 0;
}
GUEST_ABI std::int64_t ShadowSetup(std::uint64_t,const std::uint64_t* saved) {
    const auto block=saved[4];const float k=BbSettings::Get().graphics_shadow_scale;
    std::uint8_t held=1;
    if (!block || k==1 || !Read(block+kShadowHeld-kShadowBlock,&held) || held) return 0;
    for (const auto offset:kShadowScaled) { float v=0;if (Read(block+offset,&v) && std::isfinite(v)) Write(block+offset,v*k); }
    return 0;
}
GUEST_ABI std::int64_t YebisRecord(std::uint64_t,const std::uint64_t* saved) {
    const auto object=saved[5];std::uint8_t held=1;
    if (!object || !g_post_block || !Read(object+kYebisHeld,&held) || held) return 0;
    const auto& s=BbSettings::Get();
    const auto block=reinterpret_cast<std::uint64_t>(g_post_block);
    float glare=0,saturation=0,power=0;
    if (s.graphics_bloom!=1 && Read(block+kGlareLuminance,&glare) && std::isfinite(glare)) Write(block+kGlareLuminance,glare*s.graphics_bloom);
    if (s.graphics_saturation!=1 && Read(block+kSaturation,&saturation) && std::isfinite(saturation)) Write(block+kSaturation,saturation*s.graphics_saturation);
    if (!s.graphics_vignette && Read(block+kVignettePower,&power)) Write(block+kVignettePower,0.0f);
    return 0;
}
}
bool Enabled() { return enabled; }
bool Install(std::uint8_t* image,std::size_t size) {
    if (!BbSettings::Get().graphics_controls) return false;
    Hook hooks[4];
    if (size<0x55406e0+0x370 || !Prepare(hooks[0],image,size,kRenderView-0x400000,kRenderViewPrologue,RenderView) ||
        !Prepare(hooks[1],image,size,kYebisRecord-0x400000,kYebisRecordPrologue,YebisRecord) ||
        !Prepare(hooks[2],image,size,kSsaoDraw-0x400000,kSsaoDrawPrologue,AoDraw) ||
        !Prepare(hooks[3],image,size,kShadowSetup-0x400000,kShadowSetupPrologue,ShadowSetup)) {
        for (auto& h:hooks) Discard(h);
        host_log("live graphics disabled: engine bytes differ; all original paths retained");return false;
    }
    g_post_block=image+0x55406e0;
    Commit(hooks);enabled=true;
    host_log("bbhost live graphics enabled: AA, SSAO, AO strength, shadow distance, bloom, saturation and vignette");
    return true;
}
}
