// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: NVIDIA DLSS Super Resolution as an alternative to FSR 3.1/4, through the NGX core the
// NVIDIA driver installs (_nvngx.dll, libnvidia-ngx.so.1), loaded at run time: no NVIDIA SDK
// headers or libraries at build time. The DLSS model itself is NVIDIA's redistributable
// nvngx_dlss.dll / libnvidia-ngx-dlss.so (tools/fetch_dlss.sh), searched next to the
// executable and in BB_DLSS_DIR.
//
// It consumes the FSR 4 frame (vk_fsr4.h): scene color, depth, motion vectors in render pixels
// (previous - current, without jitter) and the jitter offset; the output is written in General.

#pragma once

#include <memory>
#include "video_core/renderer_vulkan/vk_fsr4.h"

namespace Vulkan {

class Instance;
class Scheduler;

class DlssUpscaler {
public:
    DlssUpscaler(const Instance& instance, Scheduler& scheduler);
    ~DlssUpscaler();

    /// NGX initialized and DLSS Super Resolution is available on this GPU and driver.
    [[nodiscard]] bool Available() const noexcept;

    /// Records DLSS into `frame.cmdbuf` (the frame's preset picks the DLSS quality mode).
    /// False when DLSS cannot run; Problem() says why.
    bool Record(const Fsr4Upscaler::Frame& frame);

    [[nodiscard]] const char* Problem() const noexcept;
    [[nodiscard]] bool Fatal() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Vulkan
