// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <xxhash.h>
#include <algorithm>

#include "bbport_toggles.h"
#include "common/assert.h"
#include "common/debug.h"
#include "common/div_ceil.h"
#include "common/hash.h"
#include "common/scope_exit.h"
#include "core/emulator_settings.h"
#include "core/memory.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/page_manager.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/host_compatibility.h"
#include "video_core/texture_cache/overlap_diagnostics.h"
#include "video_core/texture_cache/texture_cache.h"
#include "video_core/texture_cache/tile_manager.h"

namespace VideoCore {

static constexpr u64 PageShift = 12;
static constexpr u64 NumFramesBeforeRemoval = 32;

namespace {
using Binding = TextureCache::BindingType;

void TraceOverlap(const ImageInfo& requested, Binding binding, ImageId id, const Image* candidate,
                  std::string_view action, std::string_view reason, int mip = -1, int slice = -1) {
    if (!ImageOverlapLogging()) {
        return;
    }

    // Detailed Logs is meant for short diagnostic captures. Skip the extremely common
    // exact-reuse path and rate-limit the rest so a loading screen cannot create a multi-GB log.
    if (action == "reuse") {
        return;
    }
    static std::atomic<u64> emitted{0};
    const u64 index = emitted.fetch_add(1, std::memory_order_relaxed);
    if (index >= 512 && (index % 1024) != 0) {
        return;
    }

    const ImageInfo empty{};
    const auto& cached = candidate ? candidate->info : empty;
    const auto requested_format = vk::to_string(requested.pixel_format);
    const auto cached_format = vk::to_string(cached.pixel_format);
    std::printf(
        "TextureDiag overlap[%llu]: action=%.*s reason=%.*s req_addr=0x%llx req_size=%llu "
        "req_fmt=%s req=%ux%ux%u mips=%u layers=%u type=%llu binding=%u "
        "candidate=%u cand_addr=0x%llx cand_size=%llu cand_fmt=%s cand=%ux%ux%u "
        "cand_mips=%u cand_layers=%u cand_type=%llu bound=%u target=%u view_mip=%d "
        "view_slice=%d\n",
        static_cast<unsigned long long>(index), static_cast<int>(action.size()), action.data(),
        static_cast<int>(reason.size()), reason.data(),
        static_cast<unsigned long long>(requested.guest_address),
        static_cast<unsigned long long>(requested.guest_size), requested_format.c_str(),
        requested.size.width, requested.size.height, requested.size.depth,
        requested.resources.levels, requested.resources.layers,
        static_cast<unsigned long long>(u64(requested.type)), u32(binding), id.index,
        static_cast<unsigned long long>(cached.guest_address),
        static_cast<unsigned long long>(cached.guest_size), cached_format.c_str(),
        cached.size.width, cached.size.height, cached.size.depth, cached.resources.levels,
        cached.resources.layers, static_cast<unsigned long long>(u64(cached.type)),
        candidate ? unsigned(candidate->binding.is_bound) : 0u,
        candidate ? unsigned(candidate->binding.is_target) : 0u, mip, slice);
    std::fflush(stdout);
}

bool ReusableFormat(const ImageInfo& requested, const ImageInfo& cached, Binding binding,
                    bool exact) {
    if (cached.pixel_format == vk::Format::eUndefined ||
        (exact && requested.pixel_format != cached.pixel_format)) {
        return false;
    }
    if (binding == Binding::DepthTarget) {
        return cached.props.is_depth && requested.props.has_stencil == cached.props.has_stencil &&
               requested.num_bits == cached.num_bits;
    }
    if (binding == Binding::Storage || binding == Binding::RenderTarget) {
        if (cached.props.is_depth) {
            return false;
        }
    }
    if (binding == Binding::Texture && cached.props.is_depth &&
        Vulkan::LiverpoolToVK::IsFormatDepthCompatible(requested.pixel_format)) {
        // ImageView promotes R16/R32 sampling to the cached depth image's actual format.
        return true;
    }
    return IsVulkanFormatCompatible(cached.pixel_format, requested.pixel_format);
}

std::string_view ReuseRejection(const ImageInfo& requested, const ImageInfo& cached, Binding binding,
                               bool exact, int& mip, int& slice) {
    mip = slice = 0;
    if (requested.guest_address != cached.guest_address) {
        mip = requested.MipOf(cached);
        slice = mip >= 0 ? requested.SliceOf(cached, mip) : -1;
        if (mip < 0 || slice < 0) {
            return "not an aligned, contained mip/slice";
        }
    }
    if (const auto reason = requested.ViewRejection(cached, {u32(mip), u32(slice)}); !reason.empty()) {
        return reason;
    }
    return ReusableFormat(requested, cached, binding, exact)
               ? std::string_view{} : "format or binding requires another backing image";
}
} // namespace

TextureCache::TextureCache(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_,
                           Vulkan::Runtime& runtime_, AmdGpu::Liverpool* liverpool_,
                           BufferCache& buffer_cache_, PageManager& tracker_)
    : instance{instance_}, scheduler{scheduler_}, runtime{runtime_}, liverpool{liverpool_},
      buffer_cache{buffer_cache_}, tracker{tracker_}, blit_helper{instance, scheduler},
      tile_manager{instance, scheduler, runtime, buffer_cache.GetStreamBuffer()},
      readback_linear_images{EmulatorSettings.IsReadbackLinearImagesEnabled()} {

    u32 max_samplers = instance.GetMaxSamplerAllocationCount();
    trigger_gc_samplers = max_samplers * 3 / 4;
    pressure_gc_samplers = max_samplers * 7 / 8;
    critical_gc_samplers = max_samplers * 15 / 16;

    // Set up garbage collection parameters.
    if (!instance.CanReportMemoryUsage()) {
        trigger_gc_memory = 0;
        pressure_gc_memory = DEFAULT_PRESSURE_GC_MEMORY;
        critical_gc_memory = DEFAULT_CRITICAL_GC_MEMORY;
        return;
    }

    const s64 device_local_memory = static_cast<s64>(instance.GetTotalMemoryBudget());
    const s64 min_spacing_expected = device_local_memory - 1_GB;
    const s64 min_spacing_critical = device_local_memory - 512_MB;
    const s64 mem_threshold = std::min<s64>(device_local_memory, TARGET_GC_THRESHOLD);
    const s64 min_vacancy_expected = (6 * mem_threshold) / 10;
    const s64 min_vacancy_critical = (2 * mem_threshold) / 10;
    pressure_gc_memory = static_cast<u64>(
        std::max<u64>(std::min(device_local_memory - min_vacancy_expected, min_spacing_expected),
                      DEFAULT_PRESSURE_GC_MEMORY));
    critical_gc_memory = static_cast<u64>(
        std::max<u64>(std::min(device_local_memory - min_vacancy_critical, min_spacing_critical),
                      DEFAULT_CRITICAL_GC_MEMORY));
    trigger_gc_memory = static_cast<u64>((device_local_memory - mem_threshold) / 2);
}

TextureCache::~TextureCache() = default;

void TextureCache::ProcessDownloadImages() {
    std::unique_lock lk{download_images_mutex};
    for (const ImageId image_id : download_images) {
        DownloadImageMemory(image_id, true);
    }
    download_images.clear();
}

void TextureCache::DownloadImageMemory(ImageId image_id, bool sync) {
    Image& image = slot_images[image_id];
    if (False(image.flags & ImageFlagBits::GpuModified)) {
        return;
    }
    const u32 download_size = image.info.pitch * image.info.size.height * image.info.size.depth *
                              image.info.resources.layers * (image.info.num_bits / 8);
    ASSERT(download_size <= image.info.guest_size);
    const auto download =
        runtime.GetStagingPool().Request(download_size, MemoryType::HostCached, 16, !sync);
    const vk::BufferImageCopy image_download = {
        .bufferOffset = download.offset,
        .bufferRowLength = image.info.pitch,
        .bufferImageHeight = image.info.size.height,
        .imageSubresource =
            {
                .aspectMask = image.info.props.is_depth ? vk::ImageAspectFlagBits::eDepth
                                                        : vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = image.info.resources.layers,
            },
        .imageOffset = {0, 0, 0},
        .imageExtent = {image.info.size.width, image.info.size.height, image.info.size.depth},
    };
    runtime.DownloadImage(&image, download.buffer, std::span{&image_download, 1});
    if (sync) {
        scheduler.Finish();
        download.Invalidate();
        Core::Memory::Instance()->TryWriteBacking(std::bit_cast<u8*>(image.info.guest_address),
                                                  download.mapped, download_size);
    } else {
        scheduler.DeferPriorityOperation(
            [this, device_addr = image.info.guest_address, download, download_size] {
                download.Invalidate();
                Core::Memory::Instance()->TryWriteBacking(std::bit_cast<u8*>(device_addr),
                                                          download.mapped, download_size);
                runtime.GetStagingPool().FreeDeferred(download);
            });
    }
}

void TextureCache::DumpImagesAt(VAddr address, const char* dir) {
    boost::container::small_vector<ImageId, 4> ids;
    {
        std::scoped_lock lock{mutex};
        ForEachImageInRegion(address, 1, [&](ImageId image_id, Image& image) {
            if (image.info.guest_address == address) {
                ids.push_back(image_id);
            }
        });
    }
    std::printf("Image dump %#llx: %zu images\n", static_cast<unsigned long long>(address),
                ids.size());
    u32 n = 0;
    for (const ImageId image_id : ids) {
        Image& image = slot_images[image_id];
        const auto format = vk::to_string(image.info.pixel_format);
        const u32 width = image.info.size.width, height = image.info.size.height;
        std::printf("  image %u: %s %ux%u tile %u, flags %#x, layers %u, mips %u, %s\n",
                    image_id.index, format.c_str(), width, height,
                    static_cast<u32>(image.info.tile_mode), static_cast<u32>(image.flags),
                    image.info.resources.layers, image.info.resources.levels,
                    image.info.props.is_depth ? "depth" : "color");
        if (image.info.props.is_block) {
            continue;
        }
        const u32 bytes_per_pixel = image.info.props.is_depth ? 4 : image.info.num_bits / 8;
        const u64 size = u64(width) * height * bytes_per_pixel;
        const auto download = runtime.GetStagingPool().Request(size, MemoryType::HostCached, 16);
        const vk::BufferImageCopy copy = {
            .bufferOffset = download.offset,
            .imageSubresource = {image.info.props.is_depth ? vk::ImageAspectFlagBits::eDepth
                                                           : vk::ImageAspectFlagBits::eColor,
                                 0, 0, 1},
            .imageExtent = {width, height, 1},
        };
        runtime.DownloadImage(&image, download.buffer, std::span{&copy, 1});
        scheduler.Finish();
        download.Invalidate();
        const std::string path = std::format("{}/img_{:x}_{}_{}x{}_{}.raw", dir, address, n++, width,
                                             height, format);
        if (FILE* f = std::fopen(path.c_str(), "wb")) {
            std::fwrite(download.mapped, 1, size, f);
            std::fclose(f);
        }
    }
}

/// bbport: the guest memory a MaybeCpuDirty check compares, the same at marking and at
/// refresh (they hashed different ranges, the whole image and its first 8x8 pixels, so the
/// first check never matched). Such an image lies within the faulting page: cheap to hash
/// whole, and a CPU write past its first pixels still counts.
u64 TextureCache::MaybeDirtyHash(const Image& image) {
    return XXH3_64bits(std::bit_cast<const u8*>(image.info.guest_address), image.info.guest_size);
}

void TextureCache::MarkAsMaybeDirty(ImageId image_id, Image& image) {
    if (image.hash == 0) {
        // Initialize hash
        image.hash = MaybeDirtyHash(image);
    }
    image.flags |= ImageFlagBits::MaybeCpuDirty;
    UntrackImage(image_id);
}

void TextureCache::InvalidateMemory(VAddr addr, size_t size) {
    std::scoped_lock lock{mutex};
    const auto pages_start = PageManager::GetPageAddr(addr);
    const auto pages_end = PageManager::GetNextPageAddr(addr + size - 1);
    ForEachImageInRegion(pages_start, pages_end - pages_start, [&](ImageId image_id, Image& image) {
        const auto image_begin = image.info.guest_address;
        const auto image_end = image.info.guest_address + image.info.guest_size;
        if (image.Overlaps(addr, size)) {
            // Modified region overlaps image, so the image was definitely accessed by this fault.
            // Untrack the image, so that the range is unprotected and the guest can write freely.
            image.flags |= ImageFlagBits::CpuDirty;
            UntrackImage(image_id);
        } else if (pages_end < image_end) {
            // This page access may or may not modify the image.
            // We should not mark it as dirty now. If it really was modified
            // it will receive more invalidations on its other pages.
            // Remove tracking from this page only.
            UntrackImageHead(image_id);
        } else if (image_begin < pages_start) {
            // This page access does not modify the image but the page should be untracked.
            // We should not mark this image as dirty now. If it really was modified
            // it will receive more invalidations on its other pages.
            UntrackImageTail(image_id);
        } else {
            // Image begins and ends on this page so it can not receive any more invalidations.
            // We will check it's hash later to see if it really was modified.
            MarkAsMaybeDirty(image_id, image);
        }
    });
}

void TextureCache::InvalidateMemoryFromGPU(VAddr address, size_t max_size) {
    std::scoped_lock lock{mutex};
    ForEachImageInRegion(address, max_size, [&](ImageId image_id, Image& image) {
        // Only consider images that match base address.
        // TODO: Maybe also consider subresources
        if (image.info.guest_address != address) {
            return;
        }
        // Ensure image is reuploaded when accessed again.
        image.flags |= ImageFlagBits::GpuDirty;
    });
}

void TextureCache::UnmapMemory(VAddr cpu_addr, size_t size) {
    std::scoped_lock lk{mutex};

    ImageIds deleted_images;
    ForEachImageInRegion(cpu_addr, size, [&](ImageId id, Image&) { deleted_images.push_back(id); });
    for (const ImageId id : deleted_images) {
        // TODO: Download image data back to host.
        FreeImage(id);
    }
}

ImageId TextureCache::ResolveDepthOverlap(const ImageInfo& requested_info, BindingType binding,
                                          ImageId cache_image_id, bool exact_fmt) {
    auto& cache_image = slot_images[cache_image_id];

    if (!cache_image.info.props.is_depth && !requested_info.props.is_depth) {
        return {};
    }

    const bool stencil_match =
        requested_info.props.has_stencil == cache_image.info.props.has_stencil;
    const bool bpp_match = requested_info.num_bits == cache_image.info.num_bits;

    // If an image in the cache has less slices we need to expand it
    bool recreate = !requested_info.ViewRejection(cache_image.info).empty() ||
                    (exact_fmt && requested_info.pixel_format != cache_image.info.pixel_format);

    switch (binding) {
    case BindingType::Texture:
        // The guest requires a depth sampled texture, but cache can offer only Rxf. Need to
        // recreate the image.
        recreate |= requested_info.props.is_depth && !cache_image.info.props.is_depth;
        break;
    case BindingType::Storage:
        // If the guest is going to use previously created depth as storage, the image needs to be
        // recreated. (TODO: Probably a case with linear rgba8 aliasing is legit)
        recreate |= cache_image.info.props.is_depth;
        break;
    case BindingType::RenderTarget:
        // Render target can have only Rxf format. If the cache contains only Dx[S8] we need to
        // re-create the image.
        ASSERT(!requested_info.props.is_depth);
        recreate |= cache_image.info.props.is_depth;
        break;
    case BindingType::DepthTarget:
        // The guest has requested previously allocated texture to be bound as a depth target.
        // In this case we need to convert Rx float to a Dx[S8] as requested
        recreate |= !cache_image.info.props.is_depth;

        // The guest is trying to bind a depth target and cache has it. Need to be sure that aspects
        // and bpp match
        recreate |= cache_image.info.props.is_depth && !(stencil_match && bpp_match);
        break;
    default:
        break;
    }

    if (recreate) {
        // Use a real covering allocation's layout. Combining independently larger mip/layer
        // counts or recomputing attachment padding can invent guest memory/shift later mips.
        // The existing color-channel -> MS depth conversion has different samples/bpp but the
        // same byte footprint; normalize only these two fields for the geometry check.
        auto source_layout = cache_image.info;
        source_layout.num_bits = requested_info.num_bits;
        source_layout.num_samples = requested_info.num_samples;
        const bool request_covers = source_layout.ViewRejection(requested_info).empty();
        const bool cache_covers = requested_info.ViewRejection(source_layout).empty();
        if (!request_covers && !cache_covers) {
            TraceOverlap(requested_info, binding, cache_image_id, &cache_image, "reject",
                         "depth conversion has incompatible guest subresource layout");
            return {};
        }
        auto new_info = request_covers ? requested_info : source_layout;
        new_info.pixel_format = requested_info.pixel_format;
        new_info.props.is_depth = requested_info.props.is_depth;
        new_info.props.has_stencil = requested_info.props.has_stencil;
        new_info.meta_info = requested_info.meta_info;
        new_info.stencil_addr = requested_info.stencil_addr;
        new_info.stencil_size = requested_info.stencil_size;
        int mip{}, slice{};
        if (!ReuseRejection(requested_info, new_info, binding, exact_fmt, mip, slice).empty()) {
            return {};
        }
        const auto new_image_id = slot_images.insert(instance, runtime, slot_image_views, new_info);
        RegisterImage(new_image_id);

        // SlotVector insertion may relocate images. Reacquire the source after allocation.
        auto& source_image = slot_images[cache_image_id];
        auto& new_image = slot_images[new_image_id];
        new_image.usage = source_image.usage;
        RefreshImage(new_image);
        RefreshImage(source_image);
        // When creating a depth buffer through overlap resolution don't clear it on first use.
        new_image.info.meta_info.htile_clear_mask = 0;
        runtime.CopyColorAndDepth(&source_image, &new_image);

        // Keep bound attachments valid until the renderer notices the replacement.
        if (source_image.binding.is_bound || source_image.binding.is_target) {
            source_image.binding.needs_rebind = 1u;
        }
        new_image.binding.is_target = source_image.binding.is_target;
        FreeImage(cache_image_id);
        TrackImage(new_image_id);
        return new_image_id;
    }

    int mip{}, slice{};
    return ReuseRejection(requested_info, cache_image.info, binding, exact_fmt, mip, slice).empty()
               ? cache_image_id : ImageId{};
}

std::tuple<ImageId, int, int> TextureCache::ResolveOverlap(const ImageInfo& image_info,
                                                         BindingType binding,
                                                         ImageId cache_image_id,
                                                         ImageId merged_image_id,
                                                         bool exact_fmt) {
    auto& cached = slot_images[cache_image_id];
    if (cache_image_id == merged_image_id || False(cached.flags & ImageFlagBits::Registered) ||
        cached.info.pixel_format == vk::Format::eUndefined) {
        // Undefined images are stencil/depth association records, not reusable Vulkan images.
        return {{}, -1, -1};
    }
    const auto reject = [&](std::string_view reason) -> std::tuple<ImageId, int, int> {
        TraceOverlap(image_info, binding, cache_image_id, &cached, "reject", reason);
        // Retain the old cheap retirement of cold aliases, but leave bound/GPU-modified
        // images to normal GC (which can write them back). Rejection must not lose GPU data.
        if (!cached.binding.is_bound && !cached.binding.is_target &&
            False(cached.flags & ImageFlagBits::GpuModified) &&
            scheduler.CurrentTick() - cached.tick_accessed_last > NumFramesBeforeRemoval) {
            FreeImage(cache_image_id);
        }
        return {{}, -1, -1};
    };

    // Once a covering image has been selected, only merge its actual child subresources.
    // Do not replace it with a later, smaller alias and lose the previously selected view.
    if (!merged_image_id && image_info.guest_address == cached.info.guest_address) {
        if (image_info.BlockDim() != cached.info.BlockDim() ||
            image_info.num_bits * image_info.num_samples !=
                cached.info.num_bits * cached.info.num_samples) {
            return reject("different block dimensions or sample footprint");
        }
        if (image_info.array_mode != cached.info.array_mode ||
            image_info.tile_mode != cached.info.tile_mode ||
            image_info.alt_tile != cached.info.alt_tile) {
            return reject("different guest tiling layout");
        }
        if (image_info.type != cached.info.type) {
            const bool volume_copy =
                (image_info.type == AmdGpu::ImageType::Color3D &&
                 cached.info.type == AmdGpu::ImageType::Color2D &&
                 image_info.size.depth == cached.info.resources.layers) ||
                (cached.info.type == AmdGpu::ImageType::Color3D &&
                 image_info.type == AmdGpu::ImageType::Color2D &&
                 cached.info.size.depth == image_info.resources.layers);
            if (volume_copy && image_info.guest_size == cached.info.guest_size &&
                image_info.resources.levels == cached.info.resources.levels &&
                image_info.IsCompatible(cached.info)) {
                TraceOverlap(image_info, binding, cache_image_id, &cached, "expand",
                             "2D/3D backing conversion");
                return {ExpandImage(image_info, cache_image_id), -1, -1};
            }
            return reject("different image type");
        }
        const bool cached_covers = cached.info.resources.Contains(image_info.resources);
        const bool request_covers = image_info.resources.Contains(cached.info.resources);
        if (!cached_covers && !request_covers) {
            // Taking a component-wise maximum here could invent guest memory beyond BOTH
            // allocations (e.g. more mips versus more layers). Preserve this legitimate alias.
            return reject("crossed mip/layer extents; neither image contains the other");
        }
        if (const auto depth = ResolveDepthOverlap(image_info, binding, cache_image_id, exact_fmt)) {
            TraceOverlap(image_info, binding, depth, &slot_images[depth],
                         depth == cache_image_id ? "view" : "expand", "depth/color binding");
            return {depth, -1, -1};
        }

        int mip{}, slice{};
        const auto reason = ReuseRejection(image_info, cached.info, binding, exact_fmt, mip, slice);
        if (reason.empty()) {
            TraceOverlap(image_info, binding, cache_image_id, &cached, "view", "covering backing");
            return {cache_image_id, mip, slice};
        }
        if (request_covers && image_info.IsCompatible(cached.info) &&
            cached.info.ViewRejection(image_info).empty() &&
            // Changing the number of layers relocates later guest mips. Only a one-mip
            // source, or an unchanged layer stride, can be preserved by CopyImage.
            (cached.info.resources.levels == 1 ||
             image_info.resources.layers == cached.info.resources.layers)) {
            TraceOverlap(image_info, binding, cache_image_id, &cached, "expand", reason);
            return {ExpandImage(image_info, cache_image_id), -1, -1};
        }
        return reject(reason);
    }

    if (!merged_image_id && image_info.guest_address > cached.info.guest_address) {
        int mip{}, slice{};
        const auto reason = ReuseRejection(image_info, cached.info, binding, exact_fmt, mip, slice);
        if (!reason.empty()) {
            return reject(reason);
        }
        TraceOverlap(image_info, binding, cache_image_id, &cached, "view",
                     "contained subresource", mip, slice);
        return {cache_image_id, mip, slice};
    }

    // Preserve a cached mip/array slice, including a currently bound render target, before
    // retiring it. Previously the target branch freed it before any larger image/copy existed.
    const int mip = cached.info.MipOf(image_info);
    const int slice = mip >= 0 ? cached.info.SliceOf(image_info, mip) : -1;
    if (mip < 0 || slice < 0 ||
        !cached.info.ViewRejection(image_info, {u32(mip), u32(slice)}).empty() ||
        !IsVulkanFormatCompatible(image_info.pixel_format, cached.info.pixel_format)) {
        return reject("cached image is not a covered child subresource");
    }
    if (!merged_image_id) {
        merged_image_id = slot_images.insert(instance, runtime, slot_image_views, image_info);
        RegisterImage(merged_image_id);
        RefreshImage(slot_images[merged_image_id]);
    }
    // Inserting the parent may relocate SlotVector storage.
    auto& source = slot_images[cache_image_id];
    auto& merged = slot_images[merged_image_id];
    RefreshImage(source);
    runtime.CopyMip(&source, &merged, mip, slice);
    if (source.binding.is_bound || source.binding.is_target) {
        source.binding.needs_rebind = 1u;
    }
    merged.binding.is_target |= source.binding.is_target;
    TraceOverlap(image_info, binding, cache_image_id, &source, "merge",
                 "copy child before retiring backing", mip, slice);
    FreeImage(cache_image_id);
    TrackImage(merged_image_id);
    return {merged_image_id, -1, -1};
}

ImageId TextureCache::ExpandImage(const ImageInfo& info, ImageId image_id) {
    const auto new_info = info;
    const auto new_image_id = slot_images.insert(instance, runtime, slot_image_views, new_info);
    RegisterImage(new_image_id);

    auto& src_image = slot_images[image_id];
    auto& new_image = slot_images[new_image_id];

    RefreshImage(new_image);
    RefreshImage(src_image);
    runtime.CopyImage(&src_image, &new_image);
    new_image.binding.is_target = src_image.binding.is_target;

    if (src_image.binding.is_bound || src_image.binding.is_target) {
        src_image.binding.needs_rebind = 1u;
    }

    FreeImage(image_id);
    TrackImage(new_image_id);
    return new_image_id;
}

ImageId TextureCache::FindImage(ImageDesc& desc, bool exact_fmt) {
    const auto& info = desc.info;
    ASSERT(info.guest_address != 0);

    std::scoped_lock lock{mutex};

    u64 key_hash = info.guest_address ^ info.guest_size << 17 ^ u64(info.pixel_format) << 40 ^
                   u64(info.size.width) << 24 ^ info.size.height ^ u64(desc.type) << 58 ^
                   u64(exact_fmt) << 63;
    key_hash = (key_hash ^ key_hash >> 33) * 0xFF51AFD7ED558CCDull;
    key_hash = (key_hash ^ key_hash >> 33) * 0xC4CEB9FE1A85EC53ull;
    key_hash ^= key_hash >> 33;
    auto& cached = find_image_cache[key_hash % find_image_cache.size()];
    // An exact match stays valid while that image is registered with the same description;
    // resolved overlaps (views into other images) only until any image registration changes.
    const auto still_exact = [&] {
        if (cached.view_mip >= 0 || cached.view_slice >= 0 || !cached.image_id ||
            !slot_images.is_allocated(cached.image_id)) {
            return false;
        }
        const Image& image = slot_images[cached.image_id];
        return True(image.flags & ImageFlagBits::Registered) &&
               image.info.guest_address == info.guest_address &&
               image.info.guest_size == info.guest_size && image.info.size == info.size &&
               image.info.pixel_format == info.pixel_format && image.info.type == info.type &&
               image.info.resources.Contains(info.resources) &&
               image.info.LayoutKey() == info.LayoutKey();
    };
    if ((cached.generation == registry_generation.load(std::memory_order_relaxed) || still_exact()) &&
        cached.address == info.guest_address &&
        cached.size == info.guest_size && cached.extent == info.size &&
        cached.format == info.pixel_format && cached.type == info.type &&
        cached.exact_fmt == exact_fmt && cached.binding == desc.type &&
        cached.levels == info.resources.levels && cached.layers == info.resources.layers &&
        cached.layout_key == info.LayoutKey() &&
        !BbToggle::Disabled(BbToggle::FindImageCache)) {
        Image& image = slot_images[cached.image_id];
        image.tick_accessed_last = scheduler.CurrentTick();
        TouchImage(image);
        if (cached.view_mip > 0) {
            desc.view_info.range.base.level += cached.view_mip;
        }
        if (cached.view_slice > 0) {
            desc.view_info.range.base.layer += cached.view_slice;
        }
        return cached.image_id;
    }

    ImageIds image_ids;
    ForEachImageInRegion(info.guest_address, info.guest_size,
                         [&](ImageId image_id, Image& image) { image_ids.push_back(image_id); });

    ImageId image_id{};

    int view_mip{-1};
    int view_slice{-1};
    // Preserve the exact-match fast path, but validate independent mip/layer coverage too.
    // Other covering parents go through resolution so modified child targets can be merged.
    for (const auto cache_id : image_ids) {
        const auto& candidate = slot_images[cache_id];
        if (candidate.info.pixel_format == vk::Format::eUndefined ||
            candidate.info.guest_address != info.guest_address ||
            candidate.info.guest_size != info.guest_size || candidate.info.size != info.size ||
            candidate.info.resources != info.resources) {
            continue;
        }
        int mip{}, slice{};
        const auto reason = ReuseRejection(info, candidate.info, desc.type, exact_fmt, mip, slice);
        if (reason.empty()) {
            image_id = cache_id;
            view_mip = mip ? mip : -1;
            view_slice = slice ? slice : -1;
            TraceOverlap(info, desc.type, cache_id, &candidate,
                         mip || slice ? "view" : "reuse", "covers requested resources", mip, slice);
            break;
        }
        TraceOverlap(info, desc.type, cache_id, &candidate, "reject reuse", reason, mip, slice);
    }

    if (!image_id) {
        for (const auto cache_id : image_ids) {
            const auto& candidate = slot_images[cache_id];
            if (cache_id == image_id || False(candidate.flags & ImageFlagBits::Registered)) {
                continue;
            }
            // Exact-format callers cannot end up with a differently formatted covering view.
            // A conversion/expansion may still create the requested format, so let it proceed.
            const auto& merged_info = image_id ? slot_images[image_id].info : info;
            const auto [resolved, mip, slice] =
                ResolveOverlap(merged_info, desc.type, cache_id, image_id, exact_fmt);
            if (!resolved || resolved == image_id) {
                continue; // Keep the original subresource location on non-matches/child merges.
            }
            int original_mip{}, original_slice{};
            const auto reason = ReuseRejection(info, slot_images[resolved].info, desc.type,
                                               exact_fmt, original_mip, original_slice);
            if (!reason.empty()) {
                TraceOverlap(info, desc.type, resolved, &slot_images[resolved], "reject result", reason);
                continue;
            }
            image_id = resolved;
            view_mip = original_mip ? original_mip : -1;
            view_slice = original_slice ? original_slice : -1;
        }
    }
    // Create and register a new image
    if (!image_id) {
        view_mip = view_slice = -1;
        TraceOverlap(info, desc.type, {}, nullptr, "create", "no candidate covers requested view");
        image_id = slot_images.insert(instance, runtime, slot_image_views, info);
        RegisterImage(image_id);
    }

    Image& image = slot_images[image_id];
    image.tick_accessed_last = scheduler.CurrentTick();
    TouchImage(image);

    // If the image requested is a subresource of the image from cache record its location.
    if (view_mip > 0) {
        desc.view_info.range.base.level += view_mip;
    }
    if (view_slice > 0) {
        desc.view_info.range.base.layer += view_slice;
    }

    cached = FindImageCacheEntry{
        .address = info.guest_address,
        .size = info.guest_size,
        .extent = info.size,
        .format = info.pixel_format,
        .type = info.type,
        .exact_fmt = exact_fmt,
        .binding = desc.type,
        .levels = info.resources.levels,
        .layers = info.resources.layers,
        .layout_key = info.LayoutKey(),
        .generation = registry_generation.load(std::memory_order_relaxed),
        .image_id = image_id,
        .view_mip = view_mip,
        .view_slice = view_slice,
    };
    return image_id;
}

ImageId TextureCache::FindImageFromRange(VAddr address, size_t size, bool ensure_valid) {
    ImageIds image_ids;
    ForEachImageInRegion(address, size, [&](ImageId image_id, Image& image) {
        if (image.info.guest_address != address) {
            return;
        }
        if (ensure_valid && !image.SafeToDownload()) {
            return;
        }
        image_ids.push_back(image_id);
    });
    if (image_ids.size() == 1) {
        // Sometimes image size might not exactly match with requested buffer size
        // If we only found 1 candidate image use it without too many questions.
        return image_ids.back();
    }
    if (!image_ids.empty()) {
        for (s32 i = 0; i < image_ids.size(); ++i) {
            Image& image = slot_images[image_ids[i]];
            if (image.info.guest_size == size) {
                return image_ids[i];
            }
        }
        LOG_WARNING(Render_Vulkan,
                    "Failed to find exact image match for copy addr={:#x}, size={:#x}", address,
                    size);
    }
    return {};
}

ImageView& TextureCache::FindTexture(ImageId image_id, const ImageDesc& desc, ViewMemo* memo,
                                     bool refresh) {
    Image& image = slot_images[image_id];
    if (desc.type == BindingType::Storage) {
        image.flags |= ImageFlagBits::GpuModified;
        if (readback_linear_images && (!image.info.props.is_tiled || image.info.size.width <= 8) &&
            image.info.guest_address != 0) {
            std::unique_lock lk{download_images_mutex};
            download_images.emplace(image_id);
        }
    }
    if (refresh) {
        UpdateImage(image_id);
    }
    if (memo && !BbToggle::Disabled(BbToggle::TextureViewMemo)) {
        if (memo->image_id == image_id && memo->backing == image.backing && memo->view_id) {
            return slot_image_views[memo->view_id];
        }
        ImageView& view = image.FindView(desc.view_info);
        const auto& ids = image.backing->image_view_ids;
        const u32 last = image.backing->last_view;
        memo->image_id = image_id;
        memo->backing = image.backing;
        memo->view_id = last < ids.size() && &slot_image_views[ids[last]] == &view ? ids[last]
                                                                                    : ids.back();
        return view;
    }
    return image.FindView(desc.view_info);
}

ImageView& TextureCache::FindRenderTarget(ImageId image_id, const ImageDesc& desc) {
    Image& image = slot_images[image_id];
    image.flags |= ImageFlagBits::GpuModified;
    if (readback_linear_images && (!image.info.props.is_tiled || image.info.size.width <= 8)) {
        std::unique_lock lk{download_images_mutex};
        download_images.emplace(image_id);
    }
    image.usage.render_target = 1u;
    UpdateImage(image_id);

    // Register meta data for this color buffer
    if (desc.info.meta_info.cmask_addr) {
        surface_metas.emplace(desc.info.meta_info.cmask_addr,
                              MetaDataInfo{.type = MetaType::CMask});
        image.info.meta_info.cmask_addr = desc.info.meta_info.cmask_addr;
    }

    if (desc.info.meta_info.fmask_addr) {
        surface_metas.emplace(desc.info.meta_info.fmask_addr,
                              MetaDataInfo{.type = MetaType::FMask});
        image.info.meta_info.fmask_addr = desc.info.meta_info.fmask_addr;
    }

    return image.FindView(desc.view_info, false);
}

ImageView& TextureCache::FindDepthTarget(ImageId image_id, const ImageDesc& desc) {
    Image& image = slot_images[image_id];
    image.flags |= ImageFlagBits::GpuModified;
    image.usage.depth_target = 1u;
    UpdateImage(image_id);

    // Register meta data for this depth buffer
    if (desc.info.meta_info.htile_addr) {
        surface_metas.emplace(desc.info.meta_info.htile_addr,
                              MetaDataInfo{.type = MetaType::HTile,
                                           .clear_mask = image.info.meta_info.htile_clear_mask});
        image.info.meta_info.htile_addr = desc.info.meta_info.htile_addr;
    }

    // If there is a stencil attachment, link depth and stencil.
    if (desc.info.stencil_addr != 0) {
        ImageId stencil_id{};
        ForEachImageInRegion(desc.info.stencil_addr, desc.info.stencil_size,
                             [&](ImageId image_id, Image& image) {
                                 if (image.info.guest_address == desc.info.stencil_addr) {
                                     stencil_id = image_id;
                                 }
                             });
        if (!stencil_id) {
            ImageInfo info{};
            info.guest_address = desc.info.stencil_addr;
            info.guest_size = desc.info.stencil_size;
            info.size = desc.info.size;
            stencil_id = slot_images.insert(instance, runtime, slot_image_views, info);
            RegisterImage(stencil_id);
        }
        Image& stencil_image = slot_images[stencil_id];
        TouchImage(stencil_image);
        stencil_image.AssociateDepth(image_id, image.image_uid);
    }

    return image.FindView(desc.view_info, false);
}

void TextureCache::RefreshImage(Image& image) {
    if (False(image.flags & ImageFlagBits::Dirty) || image.info.num_samples > 1) {
        return;
    }
    BbStats::Timer timer{BbStats::t_refresh};

    RENDERER_TRACE;
    TRACE_HINT(fmt::format("{:x}:{:x}", image.info.guest_address, image.info.guest_size));

    if (True(image.flags & ImageFlagBits::MaybeCpuDirty) &&
        False(image.flags & ImageFlagBits::CpuDirty)) {
        const u64 hash = MaybeDirtyHash(image);
        if (image.hash == hash) {
            image.flags &= ~ImageFlagBits::MaybeCpuDirty;
            return;
        }
        image.hash = hash;
    }

    const u32 num_layers = image.info.resources.layers;
    const u32 num_mips = image.info.resources.levels;
    const bool is_gpu_modified = True(image.flags & ImageFlagBits::GpuModified);
    const bool is_gpu_dirty = True(image.flags & ImageFlagBits::GpuDirty);

    BbStats::image_upload_bytes.fetch_add(image.info.guest_size, std::memory_order_relaxed);
    boost::container::small_vector<vk::BufferImageCopy, 14> image_copies;
    for (u32 m = 0; m < num_mips; m++) {
        const u32 width = std::max(image.info.size.width >> m, 1u);
        const u32 height = std::max(image.info.size.height >> m, 1u);
        const u32 depth =
            image.info.props.is_volume ? std::max(image.info.size.depth >> m, 1u) : 1u;
        const auto [mip_size, mip_pitch, mip_height, mip_offset] = image.info.mips_layout[m];

        // Protect GPU modified resources from accidental CPU reuploads.
        // bbport: every upload records the guest memory it saw, not only uploads of images
        // the GPU had already written. An image the GPU writes after an upload from the buffer
        // cache (GpuDirty: a storage image's first binding) or from a plain texture otherwise
        // had no reference, and the first CPU write anywhere in its page replaced the GPU's
        // contents with stale guest memory (a 1x1 exposure texture computed once: the
        // character creation preview went black after one frame).
        const u8* mip_addr = std::bit_cast<u8*>(image.info.guest_address) + mip_offset;
        const u64 mip_hash = XXH3_64bits(mip_addr, mip_size);
        if (is_gpu_modified && !is_gpu_dirty && image.mip_hashes[m] == mip_hash) {
            continue;
        }
        image.mip_hashes[m] = mip_hash;

        const u32 extent_width = mip_pitch ? std::min(mip_pitch, width) : width;
        const u32 extent_height = mip_height ? std::min(mip_height, height) : height;
        image_copies.push_back({
            .bufferOffset = mip_offset,
            .bufferRowLength = mip_pitch,
            .bufferImageHeight = mip_height,
            .imageSubresource{
                .aspectMask = image.aspect_mask & ~vk::ImageAspectFlagBits::eStencil,
                .mipLevel = m,
                .baseArrayLayer = 0,
                .layerCount = num_layers,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {extent_width, extent_height, depth},
        });
    }

    if (image_copies.empty()) {
        image.flags &= ~ImageFlagBits::Dirty;
        return;
    }

    scheduler.EndRendering();

    const auto [in_buffer, in_offset] =
        buffer_cache.ObtainBufferForImage(image.info.guest_address, image.info.guest_size);
    const auto [buffer, offset] = tile_manager.DetileImage(in_buffer, in_offset, image.info);
    for (auto& copy : image_copies) {
        copy.bufferOffset += offset;
    }

    runtime.UploadImage(&image, buffer, image_copies);
}

vk::Sampler TextureCache::GetSampler(const AmdGpu::Sampler& sampler,
                                     AmdGpu::BorderColorBuffer border_color_base,
                                     const bool is_depth, float extra_lod_bias) {
    // Compare and plain uses of one S# need separate samplers; so do extra LOD biases.
    const u64 hash = HashCombine(HashCombine(XXH3_64bits(&sampler, sizeof(sampler)), is_depth),
                                 u64(std::bit_cast<u32>(extra_lod_bias)));

    std::scoped_lock lock{samplers_mutex};
    const auto [it, new_sampler] = samplers.try_emplace(hash, instance, sampler, border_color_base,
                                                        is_depth, extra_lod_bias);
    if (new_sampler) {
        samplers.at(hash).lru_id = sampler_lru_cache.Insert(hash, gc_tick);
    } else {
        sampler_lru_cache.Touch(it->second.lru_id, gc_tick);
    }

    return it->second.Handle();
}

void TextureCache::RegisterImage(ImageId image_id) {
    BbStats::images_registered.fetch_add(1, std::memory_order_relaxed);
    Image& image = slot_images[image_id];
    ASSERT_MSG(False(image.flags & ImageFlagBits::Registered),
               "Trying to register an already registered image");
    image.flags |= ImageFlagBits::Registered;
    ++registry_generation;
    total_used_memory += Common::AlignUp(image.info.guest_size, 1024);
    image.lru_id = lru_cache.Insert(image_id, gc_tick);
    image.lru_touched_tick = gc_tick;
    ForEachPage(image.info.guest_address, image.info.guest_size,
                [this, image_id](u64 page) { page_table[page].push_back(image_id); });
}

void TextureCache::UnregisterImage(ImageId image_id) {
    Image& image = slot_images[image_id];
    ASSERT_MSG(True(image.flags & ImageFlagBits::Registered),
               "Trying to unregister an already unregistered image");
    image.flags &= ~ImageFlagBits::Registered;
    ++registry_generation;
    lru_cache.Free(image.lru_id);
    total_used_memory -= Common::AlignUp(image.info.guest_size, 1024);
    ForEachPage(image.info.guest_address, image.info.guest_size, [this, image_id](u64 page) {
        const auto page_it = page_table.find(page);
        if (page_it == nullptr) {
            UNREACHABLE_MSG("Unregistering unregistered page=0x{:x}", page << PageShift);
            return;
        }
        auto& image_ids = *page_it;
        const auto vector_it = std::ranges::find(image_ids, image_id);
        if (vector_it == image_ids.end()) {
            ASSERT_MSG(false, "Unregistering unregistered image in page=0x{:x}", page << PageShift);
            return;
        }
        image_ids.erase(vector_it);
    });
}

void TextureCache::TrackImage(ImageId image_id) {
    auto& image = slot_images[image_id];
    if (!(image.flags & ImageFlagBits::Registered)) {
        return;
    }
    const auto image_begin = image.info.guest_address;
    const auto image_end = image.info.guest_address + image.info.guest_size;
    if (image_begin == image.track_addr && image_end == image.track_addr_end) {
        return;
    }

    if (!image.IsTracked()) {
        // Re-track the whole image
        image.track_addr = image_begin;
        image.track_addr_end = image_end;
        tracker.UpdatePageWatchers<1>(image_begin, image.info.guest_size);
    } else {
        if (image_begin < image.track_addr) {
            TrackImageHead(image_id);
        }
        if (image.track_addr_end < image_end) {
            TrackImageTail(image_id);
        }
    }
}

void TextureCache::TrackImageHead(ImageId image_id) {
    auto& image = slot_images[image_id];
    if (!(image.flags & ImageFlagBits::Registered)) {
        return;
    }
    const auto image_begin = image.info.guest_address;
    if (image_begin == image.track_addr) {
        return;
    }
    ASSERT(image.track_addr != 0 && image_begin < image.track_addr);
    const auto size = image.track_addr - image_begin;
    image.track_addr = image_begin;
    tracker.UpdatePageWatchers<1>(image_begin, size);
}

void TextureCache::TrackImageTail(ImageId image_id) {
    auto& image = slot_images[image_id];
    if (!(image.flags & ImageFlagBits::Registered)) {
        return;
    }
    const auto image_end = image.info.guest_address + image.info.guest_size;
    if (image_end == image.track_addr_end) {
        return;
    }
    ASSERT(image.track_addr_end != 0 && image.track_addr_end < image_end);
    const auto addr = image.track_addr_end;
    const auto size = image_end - image.track_addr_end;
    image.track_addr_end = image_end;
    tracker.UpdatePageWatchers<1>(addr, size);
}

void TextureCache::UntrackImage(ImageId image_id) {
    auto& image = slot_images[image_id];
    if (!image.IsTracked()) {
        return;
    }
    const auto addr = image.track_addr;
    const auto size = image.track_addr_end - image.track_addr;
    image.track_addr = 0;
    image.track_addr_end = 0;
    if (size != 0) {
        tracker.UpdatePageWatchers<false>(addr, size);
    }
}

void TextureCache::UntrackImageHead(ImageId image_id) {
    auto& image = slot_images[image_id];
    const auto image_begin = image.info.guest_address;
    if (!image.IsTracked() || image_begin < image.track_addr) {
        return;
    }
    const auto addr = tracker.GetNextPageAddr(image_begin);
    const auto size = addr - image_begin;
    image.track_addr = addr;
    if (image.track_addr == image.track_addr_end) {
        // This image spans only 2 pages and both are modified,
        // but the image itself was not directly affected.
        // Cehck its hash later.
        MarkAsMaybeDirty(image_id, image);
    }
    tracker.UpdatePageWatchers<false>(image_begin, size);
}

void TextureCache::UntrackImageTail(ImageId image_id) {
    auto& image = slot_images[image_id];
    const auto image_end = image.info.guest_address + image.info.guest_size;
    if (!image.IsTracked() || image.track_addr_end < image_end) {
        return;
    }
    ASSERT(image.track_addr_end != 0);
    const auto addr = tracker.GetPageAddr(image_end);
    const auto size = image_end - addr;
    image.track_addr_end = addr;
    if (image.track_addr == image.track_addr_end) {
        // This image spans only 2 pages and both are modified,
        // but the image itself was not directly affected.
        // Cehck its hash later.
        MarkAsMaybeDirty(image_id, image);
    }
    tracker.UpdatePageWatchers<false>(addr, size);
}

void TextureCache::GarbageCollectImages() {
    if (instance.CanReportMemoryUsage()) {
        total_used_memory = instance.GetDeviceMemoryUsage();
        // On integrated GPUs use the driver's current budget; a fixed startup budget caused
        // aggressive eviction of textures used only a few frames earlier.
        static const u64 forced_budget = [] {
            const char* env = std::getenv("BB_GC_BUDGET_MB");
            return env ? std::strtoull(env, nullptr, 10) << 20 : 0;
        }();
        if (instance.IsIntegrated() || forced_budget) {
            const u64 budget = forced_budget ? forced_budget : instance.GetDeviceMemoryBudgetNow();
            if (budget != 0) {
                trigger_gc_memory = budget / 10 * 7;
                pressure_gc_memory = budget / 100 * 85;
                critical_gc_memory = budget / 100 * 95;
            }
        }
    }

    // Upstream 0.3: age textures in wall-clock seconds rather than submission ticks.
    const u64 second = u64(std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count());
    if (second != gc_second) {
        gc_second = second;
        gc_tick_at_second[second % gc_tick_at_second.size()] = gc_tick;
    }

    if (total_used_memory < trigger_gc_memory) {
        return;
    }

    static const u64 idle_seconds = [] {
        const char* env = std::getenv("BB_GC_IDLE_SECONDS");
        return std::clamp<u64>(env ? std::strtoull(env, nullptr, 10) : 20, 1, 63);
    }();
    const u64 idle_tick =
        gc_tick_at_second[(second - idle_seconds) % gc_tick_at_second.size()];

    std::scoped_lock lock{mutex};
    bool pressured = false;
    bool aggresive = false;
    u64 below_tick = 0;
    size_t num_deletions = 0;
    u32 visited = 0;

    const auto configure = [&](bool allow_aggressive) {
        pressured = total_used_memory >= pressure_gc_memory;
        aggresive = allow_aggressive && total_used_memory >= critical_gc_memory;
        const u64 ticks_to_destroy =
            std::min<u64>(aggresive ? 160 : pressured ? 80 : 16, gc_tick);
        below_tick = gc_tick - ticks_to_destroy;
        if (!pressured && !aggresive) {
            below_tick = std::min(below_tick, idle_tick);
        }
        num_deletions = aggresive ? 40 : pressured ? 20 : 10;
        visited = 0;
    };

    const auto clean_up = [&](ImageId image_id) {
        if (num_deletions == 0 || ++visited > 256) {
            return true;
        }
        auto& image = slot_images[image_id];
        const bool download = image.SafeToDownload();
        const bool tiled = image.info.IsTiled();

        // Upstream 0.3: images that cannot be freed this pass must not consume the deletion
        // budget or permanently sit at the front of the LRU queue.
        if ((tiled && download) || (download && !pressured)) {
            lru_cache.Touch(image.lru_id, gc_tick);
            return false;
        }

        --num_deletions;
        if (download) {
            // Keep the existing synchronous write-back while guest pages are still protected.
            DownloadImageMemory(image_id, true);
            ++gc_downloads;
        }
        ++gc_evictions;
        FreeImage(image_id);

        if (total_used_memory < critical_gc_memory) {
            if (aggresive) {
                num_deletions >>= 2;
                aggresive = false;
                return false;
            }
            if (pressured && total_used_memory < pressure_gc_memory) {
                num_deletions >>= 1;
                pressured = false;
            }
        }
        return false;
    };

    configure(false);
    lru_cache.ForEachItemBelow(below_tick, clean_up);

    if (total_used_memory >= critical_gc_memory) {
        configure(true);
        lru_cache.ForEachItemBelow(below_tick, clean_up);
    }

    if (pressured || gc_downloads != 0) {
        const auto now = std::chrono::steady_clock::now();
        if (now - gc_report_time >= std::chrono::seconds(5)) {
            std::printf("Texture cache: memory pressure, %llu of %llu MiB (critical %llu): "
                        "%llu images evicted, %llu written back since the last report\n",
                        (unsigned long long)(total_used_memory >> 20),
                        (unsigned long long)(pressure_gc_memory >> 20),
                        (unsigned long long)(critical_gc_memory >> 20),
                        (unsigned long long)gc_evictions, (unsigned long long)gc_downloads);
            gc_report_time = now;
            gc_evictions = gc_downloads = 0;
        }
    }
}

void TextureCache::GarbageCollectSamplers() {
    total_used_samplers = samplers.size();
    if (total_used_samplers < trigger_gc_samplers) {
        return;
    }
    std::scoped_lock lock{samplers_mutex};
    bool pressured = false;
    bool aggresive = false;
    u64 ticks_to_destroy = 0;
    size_t num_deletions = 0;

    const auto configure = [&](bool allow_aggressive) {
        pressured = total_used_samplers >= pressure_gc_samplers;
        aggresive = allow_aggressive && total_used_samplers >= critical_gc_samplers;
        ticks_to_destroy = aggresive ? 160 : pressured ? 80 : 16;
        ticks_to_destroy = std::min(ticks_to_destroy, gc_tick);
        num_deletions = aggresive ? 40 : pressured ? 20 : 10;
    };
    const auto clean_up = [&](u64 hash) {
        if (num_deletions == 0) {
            return true;
        }
        --num_deletions;
        const size_t lru_id = samplers.at(hash).lru_id;
        samplers.erase(hash);
        sampler_lru_cache.Free(lru_id);
        return false;
    };

    // Try to remove anything old enough and not high priority.
    configure(false);
    sampler_lru_cache.ForEachItemBelow(gc_tick - ticks_to_destroy, clean_up);

    if (total_used_samplers >= critical_gc_samplers) {
        // If we are still over the critical limit, run an aggressive GC
        configure(true);
        sampler_lru_cache.ForEachItemBelow(gc_tick - ticks_to_destroy, clean_up);
    }
}

void TextureCache::RunGarbageCollector() {
    SCOPE_EXIT {
        ++gc_tick;
    };

    GarbageCollectImages();
    GarbageCollectSamplers();
}

void TextureCache::TouchImage(const Image& image) {
    if (image.lru_touched_tick == gc_tick) {
        return;
    }
    image.lru_touched_tick = gc_tick;
    lru_cache.Touch(image.lru_id, gc_tick);
}

void TextureCache::DeleteImage(ImageId image_id) {
    Image& image = slot_images[image_id];
    ASSERT_MSG(!image.IsTracked(), "Image was not untracked");
    ASSERT_MSG(False(image.flags & ImageFlagBits::Registered), "Image was not unregistered");

    // Remove any registered meta areas.
    const auto& meta_info = image.info.meta_info;
    if (meta_info.cmask_addr) {
        surface_metas.erase(meta_info.cmask_addr);
    }
    if (meta_info.fmask_addr) {
        surface_metas.erase(meta_info.fmask_addr);
    }
    if (meta_info.htile_addr) {
        surface_metas.erase(meta_info.htile_addr);
    }

    {
        std::unique_lock lk{download_images_mutex};
        if (download_images.contains(image_id)) {
            download_images.erase(image_id);
        }
    }

    // Reclaim image and any image views it references.
    scheduler.DeferOperation([this, image_id] {
        Image& image = slot_images[image_id];
        for (auto& backing : image.backing_images) {
            for (const ImageViewId image_view_id : backing.image_view_ids) {
                slot_image_views.erase(image_view_id);
            }
        }
        slot_images.erase(image_id);
    });
}

} // namespace VideoCore
