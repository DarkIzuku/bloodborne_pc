// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "video_core/renderer_vulkan/vk_dlss.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "bbport_settings.h"

namespace Vulkan {

namespace {

// The NGX interface, as the driver's core exports it (NVIDIA DLSS SDK 310.x, NGX API 1.5).
// Calls are cdecl, which is the platform calling convention on x86-64.
using NgxResult = unsigned int;
constexpr bool Failed(NgxResult r) {
    return (r & 0xFFF00000u) == 0xBAD00000u;
}
constexpr unsigned kNgxApiVersion = 0x15;
constexpr int kFeatureSuperSampling = 1;
constexpr unsigned long long kApplicationId = 231313132; // NVIDIA's id for unregistered apps

enum DlssFlags : int {
    FlagIsHdr = 1 << 0,
    FlagMvLowRes = 1 << 1,
    FlagAutoExposure = 1 << 6,
};
// NVSDK_NGX_PerfQuality_Value
enum PerfQuality : int { MaxPerf = 0, Balanced = 1, MaxQuality = 2, UltraPerformance = 3, Dlaa = 5 };

struct NgxParameter; ///< NVSDK_NGX_Parameter: a C++ interface implemented by the core
struct NgxHandle;

struct PathListInfo {
    const wchar_t* const* path;
    unsigned int length;
};
using LogCallback = void (*)(const char* message, int level, int feature);
struct LoggingInfo {
    LogCallback callback;
    int minimum_level; ///< 0 off, 1 on, 2 verbose
    bool disable_other_sinks;
};
struct FeatureCommonInfo {
    PathListInfo path_list;
    void* internal_data;
    LoggingInfo logging;
};

struct ImageViewInfoVk {
    VkImageView image_view;
    VkImage image;
    VkImageSubresourceRange subresource_range;
    VkFormat format;
    unsigned int width;
    unsigned int height;
};
struct BufferInfoVk {
    VkBuffer buffer;
    unsigned int size_in_bytes;
};
struct ResourceVk {
    union {
        ImageViewInfoVk image_view_info;
        BufferInfoVk buffer_info;
    } resource;
    int type; ///< 0 image view, 1 buffer
    bool read_write;
};

using PFN_InitExt2 = NgxResult (*)(unsigned long long app_id, const wchar_t* data_path,
                                   VkInstance, VkPhysicalDevice, VkDevice, PFN_vkGetInstanceProcAddr,
                                   PFN_vkGetDeviceProcAddr, unsigned version,
                                   const FeatureCommonInfo*);
using PFN_GetParameters = NgxResult (*)(NgxParameter**);
using PFN_DestroyParameters = NgxResult (*)(NgxParameter*);
using PFN_CreateFeature1 = NgxResult (*)(VkDevice, VkCommandBuffer, int feature, NgxParameter*,
                                         NgxHandle**);
using PFN_EvaluateFeature = NgxResult (*)(VkCommandBuffer, const NgxHandle*, const NgxParameter*,
                                          void* progress_callback);
using PFN_ReleaseFeature = NgxResult (*)(NgxHandle*);
using PFN_Shutdown1 = NgxResult (*)(VkDevice);

// NVSDK_NGX_Parameter declares Set and Get overloads for unsigned long long, float, double,
// unsigned int, int, two D3D resource pointers and void*, then Reset. Calls go through its
// vtable: MSVC (the Windows core) lays an overload set out in reverse order of declaration,
// the Itanium ABI (Linux core) in declaration order. This build may use either ABI itself, so
// the slots are fixed here rather than taken from a C++ declaration.
#ifdef _WIN32
enum ParamSlot : int { SetVoid = 0, SetInt = 3, SetUint = 4, SetFloat = 6,
                       GetVoid = 8, GetInt = 11, GetUint = 12, GetFloat = 14 };
#else
enum ParamSlot : int { SetFloat = 1, SetUint = 3, SetInt = 4, SetVoid = 7,
                       GetFloat = 9, GetUint = 11, GetInt = 12, GetVoid = 15 };
#endif

template <typename Fn>
Fn Slot(NgxParameter* p, int slot) {
    return reinterpret_cast<Fn>((*reinterpret_cast<void***>(p))[slot]);
}
void SetI(NgxParameter* p, const char* name, int v) {
    Slot<void (*)(NgxParameter*, const char*, int)>(p, SetInt)(p, name, v);
}
void SetUI(NgxParameter* p, const char* name, unsigned v) {
    Slot<void (*)(NgxParameter*, const char*, unsigned)>(p, SetUint)(p, name, v);
}
void SetF(NgxParameter* p, const char* name, float v) {
    Slot<void (*)(NgxParameter*, const char*, float)>(p, SetFloat)(p, name, v);
}
void SetP(NgxParameter* p, const char* name, void* v) {
    Slot<void (*)(NgxParameter*, const char*, void*)>(p, SetVoid)(p, name, v);
}
NgxResult GetI(NgxParameter* p, const char* name, int* v) {
    return Slot<NgxResult (*)(NgxParameter*, const char*, int*)>(p, GetInt)(p, name, v);
}
NgxResult GetUI(NgxParameter* p, const char* name, unsigned* v) {
    return Slot<NgxResult (*)(NgxParameter*, const char*, unsigned*)>(p, GetUint)(p, name, v);
}
NgxResult GetF(NgxParameter* p, const char* name, float* v) {
    return Slot<NgxResult (*)(NgxParameter*, const char*, float*)>(p, GetFloat)(p, name, v);
}

void Log(const char* message, int, int) {
    std::printf("DLSS: NGX %s%s", message,
                *message && message[std::strlen(message) - 1] == '\n' ? "" : "\n");
}

bool IsFloatFormat(vk::Format f) {
    switch (f) {
    case vk::Format::eR16G16B16A16Sfloat:
    case vk::Format::eR32G32B32A32Sfloat:
    case vk::Format::eB10G11R11UfloatPack32:
    case vk::Format::eR16G16B16Sfloat:
        return true;
    default:
        return false;
    }
}

int Quality(int preset) {
    switch (preset) {
    case BbSettings::NativeAA:
        return Dlaa;
    case BbSettings::Quality:
        return MaxQuality;
    case BbSettings::Balanced:
        return Balanced;
    case BbSettings::Performance:
        return MaxPerf;
    default:
        return UltraPerformance;
    }
}

ResourceVk Resource(const Fsr4Upscaler::Image& image, vk::ImageAspectFlags aspect, bool rw) {
    ResourceVk r{};
    r.resource.image_view_info = {
        .image_view = static_cast<VkImageView>(image.view),
        .image = static_cast<VkImage>(image.image),
        .subresource_range = {static_cast<VkImageAspectFlags>(aspect), 0, 1, 0, 1},
        .format = static_cast<VkFormat>(image.format),
        .width = image.width,
        .height = image.height,
    };
    r.type = 0;
    r.read_write = rw;
    return r;
}

} // namespace

struct DlssUpscaler::Impl {
    const Instance& instance;
    Scheduler& scheduler;
    bool available = false;
    bool fatal = false;
    std::string problem;

#ifdef _WIN32
    HMODULE core = nullptr;
#else
    void* core = nullptr;
#endif
    PFN_InitExt2 init = nullptr;
    PFN_GetParameters get_capability_parameters = nullptr;
    PFN_GetParameters allocate_parameters = nullptr;
    PFN_DestroyParameters destroy_parameters = nullptr;
    PFN_CreateFeature1 create_feature = nullptr;
    PFN_EvaluateFeature evaluate_feature = nullptr;
    PFN_ReleaseFeature release_feature = nullptr;
    PFN_Shutdown1 shutdown = nullptr;

    bool initialized = false;
    NgxParameter* capabilities = nullptr;
    NgxParameter* params = nullptr;
    NgxHandle* feature = nullptr;
    struct Key {
        u32 render_width, render_height, out_width, out_height;
        int quality;
        bool hdr;
        bool operator==(const Key&) const = default;
    } key{};

    Impl(const Instance& instance_, Scheduler& scheduler_)
        : instance{instance_}, scheduler{scheduler_} {}

    void Fail(std::string why, bool permanent) {
        if (problem != why) {
            std::printf("DLSS: %s\n", why.c_str());
        }
        problem = std::move(why);
        fatal = fatal || permanent;
    }

    template <typename Fn>
    bool Load(Fn& fn, const char* name) {
#ifdef _WIN32
        fn = reinterpret_cast<Fn>(GetProcAddress(core, name));
#else
        fn = reinterpret_cast<Fn>(dlsym(core, name));
#endif
        return fn != nullptr;
    }

    bool LoadCore() {
#ifdef _WIN32
        // The driver records its NGX core under this key (nvsdk_ngx_loader.h does the same).
        wchar_t dir[MAX_PATH];
        DWORD size = sizeof(dir);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore",
                         L"FullPath", RRF_RT_REG_SZ, nullptr, dir, &size) == ERROR_SUCCESS) {
            core = LoadLibraryW((std::wstring(dir) + L"\\_nvngx.dll").c_str());
        }
#else
        core = dlopen("libnvidia-ngx.so.1", RTLD_NOW);
#endif
        if (!core) {
            Fail("NVIDIA NGX core not found (an NVIDIA driver with DLSS support is needed)", true);
            return false;
        }
        if (!Load(init, "NVSDK_NGX_VULKAN_Init_Ext2") ||
            !Load(get_capability_parameters, "NVSDK_NGX_VULKAN_GetCapabilityParameters") ||
            !Load(allocate_parameters, "NVSDK_NGX_VULKAN_AllocateParameters") ||
            !Load(destroy_parameters, "NVSDK_NGX_VULKAN_DestroyParameters") ||
            !Load(create_feature, "NVSDK_NGX_VULKAN_CreateFeature1") ||
            !Load(evaluate_feature, "NVSDK_NGX_VULKAN_EvaluateFeature") ||
            !Load(release_feature, "NVSDK_NGX_VULKAN_ReleaseFeature") ||
            !Load(shutdown, "NVSDK_NGX_VULKAN_Shutdown1")) {
            Fail("the NVIDIA NGX core lacks the Vulkan entry points", true);
            return false;
        }
        return true;
    }

    /// Directories searched for nvngx_dlss: BB_DLSS_DIR, then the executable's directory.
    std::vector<std::wstring> SearchPaths() {
        std::vector<std::wstring> paths;
        if (const char* env = std::getenv("BB_DLSS_DIR"); env && *env) {
            paths.push_back(std::filesystem::path(env).wstring());
        }
#ifdef _WIN32
        wchar_t exe[MAX_PATH];
        if (GetModuleFileNameW(nullptr, exe, MAX_PATH)) {
            paths.push_back(std::filesystem::path(exe).parent_path().wstring());
        }
#else
        std::error_code ec;
        const auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
        if (!ec) {
            paths.push_back(exe.parent_path().wstring());
        }
#endif
        return paths;
    }

    /// Checks the parameter vtable slots: values set through it read back unchanged.
    bool ParameterAbiOk(NgxParameter* p) {
        SetI(p, "bbport.Probe.I", 0x5A5A);
        SetUI(p, "bbport.Probe.UI", 77u);
        SetF(p, "bbport.Probe.F", 1.25f);
        int i = 0;
        unsigned ui = 0;
        float f = 0.0f;
        return !Failed(GetI(p, "bbport.Probe.I", &i)) && i == 0x5A5A &&
               !Failed(GetUI(p, "bbport.Probe.UI", &ui)) && ui == 77u &&
               !Failed(GetF(p, "bbport.Probe.F", &f)) && f == 1.25f;
    }

    void Initialize() {
        if (!instance.IsDlssCapable()) {
            Fail("the Vulkan device lacks VK_NVX_binary_import / VK_NVX_image_view_handle "
                 "(DLSS needs an NVIDIA RTX GPU)",
                 true);
            return;
        }
        if (!LoadCore()) {
            return;
        }
        std::error_code ec;
        const auto data_dir = std::filesystem::temp_directory_path(ec) / "bbport-ngx";
        std::filesystem::create_directories(data_dir, ec);
        const std::wstring data_path = data_dir.wstring();
        const auto paths = SearchPaths();
        std::vector<const wchar_t*> path_ptrs;
        for (const auto& p : paths) {
            path_ptrs.push_back(p.c_str());
        }
        const FeatureCommonInfo info{
            .path_list = {path_ptrs.data(), static_cast<unsigned>(path_ptrs.size())},
            .internal_data = nullptr,
            .logging = {&Log, std::getenv("BB_DLSS_LOG") ? 2 : 0, false},
        };
        const auto& dispatcher = VULKAN_HPP_DEFAULT_DISPATCHER;
        const NgxResult r =
            init(kApplicationId, data_path.c_str(), VkInstance(instance.GetInstance()),
                 VkPhysicalDevice(instance.GetPhysicalDevice()), VkDevice(instance.GetDevice()),
                 dispatcher.vkGetInstanceProcAddr, dispatcher.vkGetDeviceProcAddr, kNgxApiVersion,
                 &info);
        if (Failed(r)) {
            Fail(fmt_hex("NGX initialization failed", r), true);
            return;
        }
        initialized = true;
        if (Failed(get_capability_parameters(&capabilities)) || !capabilities) {
            Fail("NGX capability query failed", true);
            return;
        }
        if (!ParameterAbiOk(capabilities)) {
            Fail("NGX parameter interface mismatch (unexpected vtable layout)", true);
            return;
        }
        int dlss_available = 0, needs_driver = 0;
        GetI(capabilities, "SuperSampling.NeedsUpdatedDriver", &needs_driver);
        if (Failed(GetI(capabilities, "SuperSampling.Available", &dlss_available)) ||
            !dlss_available) {
            int init_result = 0;
            GetI(capabilities, "SuperSampling.FeatureInitResult", &init_result);
            Fail(needs_driver ? "DLSS needs a newer NVIDIA driver"
                              : fmt_hex("DLSS unavailable (nvngx_dlss not found next to the "
                                        "executable or in BB_DLSS_DIR; tools/fetch_dlss.sh)",
                                        unsigned(init_result)),
                 true);
            return;
        }
        if (Failed(allocate_parameters(&params)) || !params) {
            Fail("NGX parameter allocation failed", true);
            return;
        }
        available = true;
        std::printf("DLSS: Super Resolution available (NGX core loaded)\n");
    }

    static std::string fmt_hex(const char* what, unsigned value) {
        char text[256];
        std::snprintf(text, sizeof(text), "%s (0x%08x)", what, value);
        return text;
    }

    bool Record(const Fsr4Upscaler::Frame& f) {
        if (!available) {
            return false;
        }
        const Key wanted{f.render_width, f.render_height, f.output.width, f.output.height,
                         Quality(f.preset), IsFloatFormat(f.color.format)};
        if (!feature || !(wanted == key)) {
            if (feature) {
                scheduler.Finish(); // the feature's resources may still be in use
                release_feature(feature);
                feature = nullptr;
            }
            SetUI(params, "CreationNodeMask", 1);
            SetUI(params, "VisibilityNodeMask", 1);
            SetUI(params, "Width", wanted.render_width);
            SetUI(params, "Height", wanted.render_height);
            SetUI(params, "OutWidth", wanted.out_width);
            SetUI(params, "OutHeight", wanted.out_height);
            SetI(params, "PerfQualityValue", wanted.quality);
            // Motion vectors at render resolution, without jitter; standard depth.
            int flags = FlagMvLowRes;
            if (wanted.hdr) {
                flags |= FlagIsHdr | FlagAutoExposure;
            }
            SetI(params, "DLSS.Feature.Create.Flags", flags);
            SetI(params, "DLSS.Enable.Output.Subrects", 0);
            const NgxResult r = create_feature(VkDevice(instance.GetDevice()),
                                               VkCommandBuffer(f.cmdbuf), kFeatureSuperSampling,
                                               params, &feature);
            if (Failed(r) || !feature) {
                feature = nullptr;
                Fail(fmt_hex("DLSS feature creation failed", r), false);
                return false;
            }
            key = wanted;
            std::printf("DLSS: %ux%u -> %ux%u, quality mode %d, %s input\n", key.render_width,
                        key.render_height, key.out_width, key.out_height, key.quality,
                        key.hdr ? "HDR" : "LDR");
        }
        ResourceVk color = Resource(f.color, vk::ImageAspectFlagBits::eColor, false);
        ResourceVk depth = Resource(f.depth, vk::ImageAspectFlagBits::eDepth, false);
        ResourceVk motion = Resource(f.motion, vk::ImageAspectFlagBits::eColor, false);
        ResourceVk output = Resource(f.output, vk::ImageAspectFlagBits::eColor, true);
        SetP(params, "Color", &color);
        SetP(params, "Depth", &depth);
        SetP(params, "MotionVectors", &motion);
        SetP(params, "Output", &output);
        SetF(params, "Jitter.Offset.X", f.jitter[0]);
        SetF(params, "Jitter.Offset.Y", f.jitter[1]);
        SetF(params, "Sharpness", 0.0f);
        SetI(params, "Reset", f.reset ? 1 : 0);
        SetF(params, "MV.Scale.X", 1.0f);
        SetF(params, "MV.Scale.Y", 1.0f);
        SetUI(params, "DLSS.Render.Subrect.Dimensions.Width", f.render_width);
        SetUI(params, "DLSS.Render.Subrect.Dimensions.Height", f.render_height);
        SetF(params, "FrameTimeDeltaInMsec", f.frame_ms);
        const NgxResult r =
            evaluate_feature(VkCommandBuffer(f.cmdbuf), feature, params, nullptr);
        if (Failed(r)) {
            Fail(fmt_hex("DLSS evaluation failed", r), false);
            return false;
        }
        problem.clear();
        return true;
    }

    ~Impl() {
        if (!initialized) {
            return;
        }
        scheduler.Finish();
        if (feature) {
            release_feature(feature);
        }
        if (params) {
            destroy_parameters(params);
        }
        if (capabilities) {
            destroy_parameters(capabilities);
        }
        shutdown(VkDevice(instance.GetDevice()));
    }
};

DlssUpscaler::DlssUpscaler(const Instance& instance, Scheduler& scheduler)
    : impl{std::make_unique<Impl>(instance, scheduler)} {
    impl->Initialize();
}

DlssUpscaler::~DlssUpscaler() = default;

bool DlssUpscaler::Available() const noexcept {
    return impl->available;
}

bool DlssUpscaler::Record(const Fsr4Upscaler::Frame& frame) {
    return impl->Record(frame);
}

const char* DlssUpscaler::Problem() const noexcept {
    return impl->problem.empty() ? nullptr : impl->problem.c_str();
}

bool DlssUpscaler::Fatal() const noexcept {
    return impl->fatal;
}

} // namespace Vulkan
