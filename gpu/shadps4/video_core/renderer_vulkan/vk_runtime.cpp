// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <boost/container/small_vector.hpp>
#include <cstdlib>
#include "bbport_toggles.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/image_copy_region.h"
#include "video_core/texture_cache/overlap_diagnostics.h"
#include "video_core/renderer_vulkan/vk_gpu_profiler.h"
#include "video_core/renderer_vulkan/vk_scene_resolution.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/blit_helper.h"
#include "video_core/texture_cache/image.h"

#include <vulkan/vulkan_format_traits.hpp>

namespace Vulkan {

static vk::ImageType ConvertImageType(AmdGpu::ImageType type) noexcept {
    switch (type) {
    case AmdGpu::ImageType::Color1D:
    case AmdGpu::ImageType::Color1DArray:
        return vk::ImageType::e1D;
    case AmdGpu::ImageType::Color2D:
    case AmdGpu::ImageType::Color2DMsaa:
    case AmdGpu::ImageType::Color2DArray:
        return vk::ImageType::e2D;
    case AmdGpu::ImageType::Color3D:
        return vk::ImageType::e3D;
    default:
        UNREACHABLE_MSG("Unexpected image type {}", u32(type));
    }
}

static CopyLayers SanitizeCopyLayers(const VideoCore::ImageInfo& src,
                                     const VideoCore::ImageInfo& dst, u32 src_mip, u32 dst_mip,
                                     u32 src_base = 0, u32 dst_base = 0,
                                     u32 requested = std::numeric_limits<u32>::max()) {
    if (src_mip >= src.resources.levels || dst_mip >= dst.resources.levels ||
        src_mip >= 32 || dst_mip >= 32) {
        return {};
    }
    const bool src_3d = ConvertImageType(src.type) == vk::ImageType::e3D;
    const bool dst_3d = ConvertImageType(dst.type) == vk::ImageType::e3D;
    const auto copy = PlanCopyLayers(src_3d, dst_3d, src.resources.layers, dst.resources.layers,
                                     std::max(src.size.depth >> src_mip, 1u),
                                     std::max(dst.size.depth >> dst_mip, 1u),
                                     src_base, dst_base, requested);
    if (VideoCore::ImageOverlapLogging()) {
        LOG_INFO(Render_Vulkan,
                 "Image copy: src={:#x} size={} format={} type={} {}x{}x{} mips={} layers={} "
                 "mip={} base={} dst={:#x} size={} format={} type={} {}x{}x{} mips={} layers={} "
                 "mip={} base={} requested={} copy layers={}/{} depth={} reason={}",
                 src.guest_address, src.guest_size, vk::to_string(src.pixel_format), u64(src.type),
                 src.size.width, src.size.height, src.size.depth, src.resources.levels,
                 src.resources.layers, src_mip, src_base, dst.guest_address, dst.guest_size,
                 vk::to_string(dst.pixel_format), u64(dst.type), dst.size.width, dst.size.height,
                 dst.size.depth, dst.resources.levels, dst.resources.layers, dst_mip, dst_base,
                 requested, copy.source, copy.destination, copy.depth,
                 !copy ? "empty/out-of-range subresource" :
                 (src_3d && src.resources.layers != 1) || (dst_3d && dst.resources.layers != 1)
                     ? "sanitized invalid 3D array layer count" : "subresource intersection");
    }
    return copy;
}

static vk::Extent3D CopyExtent(const VideoCore::ImageInfo& src, u32 src_mip,
                               const VideoCore::ImageInfo& dst, u32 dst_mip, u32 depth) {
    const u32 src_block = src.props.is_block ? 4 : 1;
    const u32 dst_block = dst.props.is_block ? 4 : 1;
    const auto dimension = [&](u32 source, u32 destination) {
        const u32 src_size = std::max(source >> src_mip, 1u);
        const u32 dst_size = std::max(destination >> dst_mip, 1u);
        // VkImageCopy extent is expressed in source texels, including block-texel copies.
        return std::min(src_size, ((dst_size + dst_block - 1) / dst_block) * src_block);
    };
    return {dimension(src.size.width, dst.size.width), dimension(src.size.height, dst.size.height),
            depth};
}

static u64 BufferImageCopySize(const vk::BufferImageCopy& copy, const vk::Format pixel_format) {
    const u32 row_length = copy.bufferRowLength ? copy.bufferRowLength : copy.imageExtent.width;
    const u32 height = copy.bufferImageHeight ? copy.bufferImageHeight : copy.imageExtent.height;

    const auto block = vk::blockExtent(pixel_format);
    const u32 block_size = vk::blockSize(pixel_format);
    const u64 row_pitch = ((u64(row_length) + block[0] - 1) / block[0]) * block_size;
    const u64 slice_pitch = ((u64(height) + block[1] - 1) / block[1]) * row_pitch;

    const u32 width_in_blocks = (copy.imageExtent.width + block[0] - 1) / block[0];
    const u32 height_in_blocks = (copy.imageExtent.height + block[1] - 1) / block[1];
    const u64 num_slices = u64(copy.imageExtent.depth) * copy.imageSubresource.layerCount;

    return (num_slices - 1) * slice_pitch + (height_in_blocks - 1) * row_pitch +
           width_in_blocks * block_size;
}

Runtime::Runtime(const Instance& instance_, Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_}, staging_pool{instance_, scheduler_} {
    blit_helper = std::make_unique<VideoCore::BlitHelper>(instance, scheduler);

    memory_barrier.dstStageMask = vk::PipelineStageFlagBits2::eAllCommands;
    memory_barrier.dstAccessMask =
        vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
}

void Runtime::TickFrame() {
    staging_pool.TickFrame();
}

void Runtime::CopyBuffer(const VideoCore::Buffer* src, const VideoCore::Buffer* dst,
                         std::span<const vk::BufferCopy> copies) {
    scheduler.EndRendering();

    // bbport: many regions (HLE copy shaders) are tracked as one bounding range per buffer:
    // conservative for barriers, and two tree lookups instead of two per region.
    const bool bounded = copies.size() > 4;
    u64 src_min = ~0ULL, src_max = 0, dst_min = ~0ULL, dst_max = 0;
    if (bounded) {
        for (const auto& copy : copies) {
            src_min = std::min<u64>(src_min, copy.srcOffset);
            src_max = std::max<u64>(src_max, copy.srcOffset + copy.size);
            dst_min = std::min<u64>(dst_min, copy.dstOffset);
            dst_max = std::max<u64>(dst_max, copy.dstOffset + copy.size);
        }
    }

    bool needs_flush{};
    if (bounded) {
        needs_flush = IsBufferAccessed(src, src_min, src_max - src_min) ||
                      IsBufferAccessed(dst, dst_min, dst_max - dst_min, true);
    } else {
        for (const auto& copy : copies) {
            needs_flush |= IsBufferAccessed(src, copy.srcOffset, copy.size);
            needs_flush |= IsBufferAccessed(dst, copy.dstOffset, copy.size, true);
        }
    }
    if (needs_flush) {
        FlushBarriers();
    }

    scheduler.Record([src_handle = src->Handle(), dst_handle = dst->Handle(),
                      regions = scheduler.RecordData(copies)](vk::CommandBuffer cmdbuf) {
        cmdbuf.copyBuffer(src_handle, dst_handle, regions.size(), regions.data());
    });

    if (bounded) {
        AccessBuffer(src, src_min, src_max - src_min, vk::PipelineStageFlagBits2::eCopy,
                     vk::AccessFlagBits2::eTransferRead);
        AccessBuffer(dst, dst_min, dst_max - dst_min, vk::PipelineStageFlagBits2::eCopy,
                     vk::AccessFlagBits2::eTransferWrite);
        return;
    }
    for (const auto& copy : copies) {
        AccessBuffer(src, copy.srcOffset, copy.size, vk::PipelineStageFlagBits2::eCopy,
                     vk::AccessFlagBits2::eTransferRead);
        AccessBuffer(dst, copy.dstOffset, copy.size, vk::PipelineStageFlagBits2::eCopy,
                     vk::AccessFlagBits2::eTransferWrite);
    }
}

void Runtime::FillBuffer(const VideoCore::Buffer* dst, u64 offset, u64 size, u32 value) {
    scheduler.EndRendering();

    if (IsBufferAccessed(dst, offset, size, true)) {
        FlushBarriers();
    }

    scheduler.Record([handle = dst->Handle(), offset, size, value](vk::CommandBuffer cmdbuf) {
        cmdbuf.fillBuffer(handle, offset, size, value);
    });

    AccessBuffer(dst, offset, size, vk::PipelineStageFlagBits2::eClear,
                 vk::AccessFlagBits2::eTransferWrite);
}

void Runtime::InlineData(VideoCore::Buffer* dst, u64 offset, u32 value) {
    scheduler.EndRendering();

    if (IsBufferAccessed(dst, offset, sizeof(value), true)) {
        FlushBarriers();
    }

    scheduler.Record([handle = dst->Handle(), offset, value](vk::CommandBuffer cmdbuf) {
        cmdbuf.updateBuffer(handle, offset, sizeof(value), &value);
    });

    AccessBuffer(dst, offset, sizeof(value), vk::PipelineStageFlagBits2::eCopy,
                 vk::AccessFlagBits2::eTransferWrite);
}

bool Runtime::Transit(VideoCore::Image* image, vk::ImageLayout dst_layout,
                      vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access,
                      std::optional<VideoCore::SubresourceRange> subres_range) {
    BeforeImageAccess();
    if (scene_targets) scene_targets->NativeAccess(*image, dst_access);
    const size_t prev_num_barriers = static_cast<size_t>(image_barriers.size());
    image->GetBarriers(image_barriers, dst_layout, dst_access, dst_stage, subres_range);
    return image_barriers.size() != prev_num_barriers;
}

Runtime::TransferMark::TransferMark(Runtime& runtime, const char* what,
                                    const VideoCore::Image& image) {
    profiler = GpuProfiler::Get();
    if (!profiler || !profiler->Records(&runtime.scheduler)) {
        profiler = nullptr;
        return;
    }
    resume = profiler->Current();
    const auto& info = image.info;
    const u64 key = (u64(reinterpret_cast<uintptr_t>(what)) << 20) ^
                    (u64(info.pixel_format) << 40) ^ (u64(info.size.width) << 16) ^
                    info.size.height ^ 0x7A5Full;
    profiler->Mark(key, [&] {
        return fmt::format("{} {} {}x{}", what, vk::to_string(info.pixel_format),
                           info.size.width, info.size.height);
    });
}

Runtime::TransferMark::~TransferMark() {
    if (profiler && resume) {
        profiler->Resume(resume);
    }
}

void Runtime::UploadImage(VideoCore::Image* dst, const VideoCore::Buffer* src,
                          std::span<const vk::BufferImageCopy> upload_copies) {
    SetBackingSamples(dst, dst->info.num_samples, false);
    scheduler.EndRendering();
    const TransferMark mark{*this, "image upload", *dst};

    bool needs_flush =
        Transit(dst, vk::ImageLayout::eTransferDstOptimal, vk::PipelineStageFlagBits2::eCopy,
                vk::AccessFlagBits2::eTransferWrite);
    for (const auto& copy : upload_copies) {
        const auto copy_size = BufferImageCopySize(copy, dst->info.pixel_format);
        needs_flush |= IsBufferAccessed(src, copy.bufferOffset, copy_size);
    }
    if (needs_flush) {
        FlushBarriers();
    }

    scheduler.Record([src_handle = src->Handle(), image = dst->GetImage(),
                      regions = scheduler.RecordData(upload_copies)](vk::CommandBuffer cmdbuf) {
        cmdbuf.copyBufferToImage(src_handle, image, vk::ImageLayout::eTransferDstOptimal,
                                 regions.size(), regions.data());
    });

    for (const auto& copy : upload_copies) {
        const auto copy_size = BufferImageCopySize(copy, dst->info.pixel_format);
        AccessBuffer(src, copy.bufferOffset, copy_size, vk::PipelineStageFlagBits2::eCopy,
                     vk::AccessFlagBits2::eTransferRead);
    }

    dst->flags &= ~VideoCore::ImageFlagBits::Dirty;
}

void Runtime::DownloadImage(VideoCore::Image* src, const VideoCore::Buffer* dst,
                            std::span<const vk::BufferImageCopy> download_copies) {
    SetBackingSamples(src, src->info.num_samples);
    scheduler.EndRendering();
    const TransferMark mark{*this, "image download", *src};

    bool needs_flush =
        Transit(src, vk::ImageLayout::eTransferSrcOptimal, vk::PipelineStageFlagBits2::eCopy,
                vk::AccessFlagBits2::eTransferRead);
    for (const auto& copy : download_copies) {
        const auto copy_size = BufferImageCopySize(copy, src->info.pixel_format);
        needs_flush |= IsBufferAccessed(dst, copy.bufferOffset, copy_size, true);
    }
    if (needs_flush) {
        FlushBarriers();
    }

    scheduler.Record([image = src->GetImage(), buffer = dst->Handle(),
                      regions = scheduler.RecordData(download_copies)](vk::CommandBuffer cmdbuf) {
        cmdbuf.copyImageToBuffer(image, vk::ImageLayout::eTransferSrcOptimal, buffer,
                                 regions.size(), regions.data());
    });

    for (const auto& copy : download_copies) {
        const auto copy_size = BufferImageCopySize(copy, src->info.pixel_format);
        AccessBuffer(dst, copy.bufferOffset, copy_size, vk::PipelineStageFlagBits2::eCopy,
                     vk::AccessFlagBits2::eTransferWrite);
    }
}

void Runtime::CopyImage(VideoCore::Image* src, VideoCore::Image* dst) {
    const u32 num_mips = std::min(src->info.resources.levels, dst->info.resources.levels);

    // Format mismatch warning (safe but useful)
    if (src->info.pixel_format != dst->info.pixel_format) {
        LOG_DEBUG(Render_Vulkan,
                  "Copy between different formats: src={}, dst={}. "
                  "Result may be undefined.",
                  vk::to_string(src->info.pixel_format), vk::to_string(dst->info.pixel_format));
    }

    // Match sample count before copying
    SetBackingSamples(dst, dst->info.num_samples, false);
    SetBackingSamples(src, src->info.num_samples);

    boost::container::small_vector<vk::ImageCopy, 8> regions;

    const vk::ImageAspectFlags src_aspect = src->aspect_mask & ~vk::ImageAspectFlagBits::eStencil;
    const vk::ImageAspectFlags dst_aspect = dst->aspect_mask & ~vk::ImageAspectFlagBits::eStencil;

    for (u32 mip = 0; mip < num_mips; ++mip) {
        const auto copy = SanitizeCopyLayers(src->info, dst->info, mip, mip);
        if (!copy) {
            continue;
        }
        regions.push_back(vk::ImageCopy{
            .srcSubresource{.aspectMask = src_aspect, .mipLevel = mip, .baseArrayLayer = 0,
                            .layerCount = copy.source},
            .dstSubresource{.aspectMask = dst_aspect, .mipLevel = mip, .baseArrayLayer = 0,
                            .layerCount = copy.destination},
            .extent = CopyExtent(src->info, mip, dst->info, mip, copy.depth),
        });
    }
    if (regions.empty()) {
        return;
    }

    scheduler.EndRendering();

    bool needs_flush =
        Transit(src, vk::ImageLayout::eTransferSrcOptimal, vk::PipelineStageFlagBits2::eCopy,
                vk::AccessFlagBits2::eTransferRead);
    needs_flush |= Transit(dst, vk::ImageLayout::eTransferDstOptimal,
                           vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferWrite);
    if (needs_flush) {
        FlushBarriers();
    }

    scheduler.Record([src_image = src->GetImage(), dst_image = dst->GetImage(),
                      regions](vk::CommandBuffer cmdbuf) {
        cmdbuf.copyImage(src_image, vk::ImageLayout::eTransferSrcOptimal, dst_image,
                         vk::ImageLayout::eTransferDstOptimal, regions);
    });

    dst->flags |= (src->flags & VideoCore::ImageFlagBits::GpuModified);
    dst->flags &= ~VideoCore::ImageFlagBits::Dirty;
}

void Runtime::CopyImageWithBuffer(VideoCore::Image* src, VideoCore::Image* dst,
                                  const VideoCore::Buffer* buffer, u64 offset) {
    const u32 num_mips = std::min(src->info.resources.levels, dst->info.resources.levels);
    SetBackingSamples(dst, dst->info.num_samples, false);
    SetBackingSamples(src, src->info.num_samples);

    boost::container::small_vector<std::pair<vk::BufferImageCopy, vk::BufferImageCopy>, 8> regions;
    u64 copy_size = 0;
    for (u32 mip = 0; mip < num_mips; ++mip) {
        const auto layers = SanitizeCopyLayers(src->info, dst->info, mip, mip);
        if (!layers) {
            continue;
        }
        vk::BufferImageCopy download{
            .bufferOffset = offset,
            .imageSubresource{
                .aspectMask = src->aspect_mask & ~vk::ImageAspectFlagBits::eStencil,
                .mipLevel = mip,
                .baseArrayLayer = 0,
                .layerCount = layers.source,
            },
            .imageExtent = CopyExtent(src->info, mip, dst->info, mip,
                                     src->info.props.is_volume ? layers.depth : 1u),
        };
        auto upload = download;
        upload.imageSubresource.aspectMask = dst->aspect_mask & ~vk::ImageAspectFlagBits::eStencil;
        upload.imageSubresource.layerCount = layers.destination;
        upload.imageExtent.depth = dst->info.props.is_volume ? layers.depth : 1u;
        copy_size = std::max<u64>(copy_size, BufferImageCopySize(download, src->info.pixel_format));
        regions.emplace_back(download, upload);
    }
    if (regions.empty()) {
        return;
    }
    ASSERT(copy_size <= 128_MB && offset <= buffer->SizeBytes() &&
           copy_size <= buffer->SizeBytes() - offset);

    scheduler.EndRendering();

    bool needs_flush =
        Transit(src, vk::ImageLayout::eTransferSrcOptimal, vk::PipelineStageFlagBits2::eCopy,
                vk::AccessFlagBits2::eTransferRead);
    needs_flush |= Transit(dst, vk::ImageLayout::eTransferDstOptimal,
                           vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferWrite);
    needs_flush |= IsBufferAccessed(buffer, offset, copy_size);
    if (needs_flush) {
        FlushBarriers();
    }

    scheduler.Record([src_image = src->GetImage(), dst_image = dst->GetImage(),
                      handle = buffer->Handle(), regions](vk::CommandBuffer cmdbuf) {
        for (const auto& [download, upload] : regions) {
            cmdbuf.copyImageToBuffer(src_image, vk::ImageLayout::eTransferSrcOptimal, handle, download);
            const vk::MemoryBarrier2 written{
                .srcStageMask = vk::PipelineStageFlagBits2::eCopy,
                .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eCopy,
                .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
            };
            cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &written});
            cmdbuf.copyBufferToImage(handle, dst_image, vk::ImageLayout::eTransferDstOptimal, upload);
            // Reuse the staging range for the next mip only after the previous upload read it.
            const vk::MemoryBarrier2 consumed{
                .srcStageMask = vk::PipelineStageFlagBits2::eCopy,
                .srcAccessMask = vk::AccessFlagBits2::eTransferRead,
                .dstStageMask = vk::PipelineStageFlagBits2::eCopy,
                .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            };
            cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &consumed});
        }
    });

    dst->flags |= (src->flags & VideoCore::ImageFlagBits::GpuModified);
    dst->flags &= ~VideoCore::ImageFlagBits::Dirty;
}

void Runtime::CopyMip(VideoCore::Image* src, VideoCore::Image* dst, u32 mip, u32 slice) {
    const auto copy = SanitizeCopyLayers(src->info, dst->info, 0, mip, 0, slice);
    if (!copy || mip >= dst->info.mips_layout.size()) {
        return;
    }
    const auto dst_dim = dst->info.props.is_block ? 2 : 0;
    const auto mip_block_w = std::max(dst->info.size.width >> (mip + dst_dim), 1u);
    const auto mip_block_h = std::max(dst->info.size.height >> (mip + dst_dim), 1u);
    const auto mip_block_p = std::max(dst->info.mips_layout[mip].pitch >> dst_dim, 1u);

    const auto src_dim = src->info.props.is_block ? 2 : 0;
    ASSERT(mip_block_w == std::max(src->info.size.width >> src_dim, 1u));
    ASSERT(mip_block_h == std::max(src->info.size.height >> src_dim, 1u));
    ASSERT(mip_block_p == std::max(src->info.mips_layout[0].pitch >> src_dim, 1u));

    const vk::ImageCopy image_copy{
        .srcSubresource{
            .aspectMask = src->aspect_mask,
            .mipLevel = 0,
            .baseArrayLayer = 0,
            .layerCount = copy.source,
        },
        .dstSubresource{
            .aspectMask = src->aspect_mask,
            .mipLevel = mip,
            .baseArrayLayer = slice,
            .layerCount = copy.destination,
        },
        .extent = CopyExtent(src->info, 0, dst->info, mip, copy.depth),
    };

    SetBackingSamples(dst, dst->info.num_samples);
    SetBackingSamples(src, src->info.num_samples);

    scheduler.EndRendering();

    bool needs_flush =
        Transit(src, vk::ImageLayout::eTransferSrcOptimal, vk::PipelineStageFlagBits2::eCopy,
                vk::AccessFlagBits2::eTransferRead);
    needs_flush |= Transit(dst, vk::ImageLayout::eTransferDstOptimal,
                           vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferWrite);
    if (needs_flush) {
        FlushBarriers();
    }

    scheduler.Record([src_image = src->GetImage(), dst_image = dst->GetImage(),
                      image_copy](vk::CommandBuffer cmdbuf) {
        cmdbuf.copyImage(src_image, vk::ImageLayout::eTransferSrcOptimal, dst_image,
                         vk::ImageLayout::eTransferDstOptimal, image_copy);
    });

    dst->flags |= (src->flags & VideoCore::ImageFlagBits::GpuModified);
    dst->flags &= ~VideoCore::ImageFlagBits::Dirty;
}

void Runtime::CopyColorAndDepth(VideoCore::Image* src, VideoCore::Image* dst) {
    if (src->info.num_samples == 1 && dst->info.num_samples == 1) {
        if (instance.IsMaintenance8Supported() ||
            src->info.props.is_depth == dst->info.props.is_depth) {
            CopyImage(src, dst);
        } else {
            // Perform depth from/to color copy using the intermediate copy buffer.
            static constexpr size_t COPY_BUFFER_SIZE = 128_MB;
            const auto copy_ref =
                staging_pool.Request(COPY_BUFFER_SIZE, VideoCore::MemoryType::DeviceLocal);
            CopyImageWithBuffer(src, dst, copy_ref.buffer, copy_ref.offset);
        }
    } else if (src->info.num_samples == 1 && dst->info.num_samples > 1 &&
               dst->info.props.is_depth) {
        // Perform a rendering pass to transfer the channels of source as samples in dest.
        bool needs_flush =
            Transit(src, vk::ImageLayout::eShaderReadOnlyOptimal,
                    vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderRead);
        needs_flush |= Transit(dst, vk::ImageLayout::eDepthStencilAttachmentOptimal,
                               vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                                   vk::PipelineStageFlagBits2::eLateFragmentTests,
                               vk::AccessFlagBits2::eDepthStencilAttachmentWrite);
        if (needs_flush) {
            FlushBarriers();
        }

        blit_helper->ReinterpretColorAsMsDepth(
            dst->info.size.width, dst->info.size.height, dst->info.num_samples,
            src->info.pixel_format, dst->info.pixel_format, src->GetImage(), dst->GetImage());
    } else {
        LOG_WARNING(Render_Vulkan, "Unimplemented depth overlap copy");
    }
}

void Runtime::CopyDepthStencil(VideoCore::Image* src, VideoCore::Image* dst,
                               const VideoCore::SubresourceRange& sub_range) {
    scheduler.EndRendering();

    bool needs_flush =
        Transit(src, vk::ImageLayout::eTransferSrcOptimal, vk::PipelineStageFlagBits2::eCopy,
                vk::AccessFlagBits2::eTransferRead);
    needs_flush |= Transit(dst, vk::ImageLayout::eTransferDstOptimal,
                           vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferWrite);
    if (needs_flush) {
        FlushBarriers();
    }

    const auto aspect_mask = src->aspect_mask & dst->aspect_mask;

    const vk::ImageCopy region = {
        .srcSubresource{
            .aspectMask = aspect_mask,
            .mipLevel = 0,
            .baseArrayLayer = sub_range.base.layer,
            .layerCount = sub_range.extent.layers,
        },
        .srcOffset = {0, 0, 0},
        .dstSubresource{
            .aspectMask = aspect_mask,
            .mipLevel = 0,
            .baseArrayLayer = sub_range.base.layer,
            .layerCount = sub_range.extent.layers,
        },
        .dstOffset = {0, 0, 0},
        .extent = {dst->info.size.width, dst->info.size.height, 1},
    };

    scheduler.Record([src_image = src->GetImage(), dst_image = dst->GetImage(),
                      region](vk::CommandBuffer cmdbuf) {
        cmdbuf.copyImage(src_image, vk::ImageLayout::eTransferSrcOptimal, dst_image,
                         vk::ImageLayout::eTransferDstOptimal, region);
    });

    dst->flags |= VideoCore::ImageFlagBits::GpuModified;
    dst->flags &= ~VideoCore::ImageFlagBits::Dirty;
}

void Runtime::ResolveImage(VideoCore::Image* src, VideoCore::Image* dst,
                           const VideoCore::SubresourceRange& src_range,
                           const VideoCore::SubresourceRange& dst_range) {
    SetBackingSamples(dst, 1, false);
    scheduler.EndRendering();

    const bool needs_resolve = src->backing->num_samples != 1;
    const auto dst_stage =
        needs_resolve ? vk::PipelineStageFlagBits2::eResolve : vk::PipelineStageFlagBits2::eCopy;

    bool needs_flush = Transit(src, vk::ImageLayout::eTransferSrcOptimal, dst_stage,
                               vk::AccessFlagBits2::eTransferRead);
    needs_flush |= Transit(dst, vk::ImageLayout::eTransferDstOptimal, dst_stage,
                           vk::AccessFlagBits2::eTransferWrite);
    if (needs_flush) {
        FlushBarriers();
    }

    const auto copy = SanitizeCopyLayers(src->info, dst->info, src_range.base.level,
                                         dst_range.base.level, src_range.base.layer,
                                         dst_range.base.layer,
                                         std::min(src_range.extent.layers, dst_range.extent.layers));
    if (!copy) {
        return;
    }
    if (!needs_resolve) {
        const vk::ImageCopy region = {
            .srcSubresource{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = src_range.base.level,
                .baseArrayLayer = src_range.base.layer,
                .layerCount = copy.source,
            },
            .srcOffset = {0, 0, 0},
            .dstSubresource{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = dst_range.base.level,
                .baseArrayLayer = dst_range.base.layer,
                .layerCount = copy.destination,
            },
            .dstOffset = {0, 0, 0},
            .extent = CopyExtent(src->info, src_range.base.level, dst->info, dst_range.base.level,
                                 copy.depth),
        };
        scheduler.Record([src_image = src->GetImage(), dst_image = dst->GetImage(),
                          region](vk::CommandBuffer cmdbuf) {
            cmdbuf.copyImage(src_image, vk::ImageLayout::eTransferSrcOptimal, dst_image,
                             vk::ImageLayout::eTransferDstOptimal, region);
        });
    } else {
        const vk::ImageResolve region = {
            .srcSubresource{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = src_range.base.level,
                .baseArrayLayer = src_range.base.layer,
                .layerCount = copy.source,
            },
            .srcOffset = {0, 0, 0},
            .dstSubresource{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = dst_range.base.level,
                .baseArrayLayer = dst_range.base.layer,
                .layerCount = copy.destination,
            },
            .dstOffset = {0, 0, 0},
            .extent = CopyExtent(src->info, src_range.base.level, dst->info, dst_range.base.level,
                                 copy.depth),
        };
        scheduler.Record([src_image = src->GetImage(), dst_image = dst->GetImage(),
                          region](vk::CommandBuffer cmdbuf) {
            cmdbuf.resolveImage(src_image, vk::ImageLayout::eTransferSrcOptimal, dst_image,
                                vk::ImageLayout::eTransferDstOptimal, region);
        });
    }

    dst->flags |= VideoCore::ImageFlagBits::GpuModified;
    dst->flags &= ~VideoCore::ImageFlagBits::Dirty;
}

void Runtime::ClearImage(VideoCore::Image* dst, const VideoCore::SubresourceRange& range,
                         const vk::ClearValue& clear_value) {
    scheduler.EndRendering();

    const bool needs_flush =
        Transit(dst, vk::ImageLayout::eTransferDstOptimal, vk::PipelineStageFlagBits2::eClear,
                vk::AccessFlagBits2::eTransferWrite, range);
    if (needs_flush) {
        FlushBarriers();
    }

    const vk::ImageSubresourceRange vk_range = {
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = range.base.level,
        .levelCount = range.extent.levels,
        .baseArrayLayer = range.base.layer,
        .layerCount = range.extent.layers,
    };
    scheduler.Record([image = dst->GetImage(), color = clear_value.color,
                      vk_range](vk::CommandBuffer cmdbuf) {
        cmdbuf.clearColorImage(image, vk::ImageLayout::eTransferDstOptimal, color, vk_range);
    });

    dst->flags |= VideoCore::ImageFlagBits::GpuModified;
    dst->flags &= ~VideoCore::ImageFlagBits::Dirty;
}

void Runtime::SetBackingSamples(VideoCore::Image* image, u32 num_samples, bool copy_backing) {
    BeforeImageAccess();
    auto& backing = image->backing;
    auto& backing_images = image->backing_images;
    const auto& info = image->info;
    if (!backing || backing->num_samples == num_samples) {
        return;
    }
    ASSERT_MSG(!image->info.props.is_depth, "Swapping samples is only valid for color images");
    VideoCore::Image::BackingImage* new_backing;
    auto it = std::ranges::find(backing_images, num_samples,
                                &VideoCore::Image::BackingImage::num_samples);
    if (it == backing_images.end()) {
        auto new_image_ci = backing->image.image_ci;
        new_image_ci.samples = LiverpoolToVK::NumSamples(num_samples, image->supported_samples);

        new_backing = &backing_images.emplace_back();
        new_backing->num_samples = num_samples;
        new_backing->image = VideoCore::UniqueImage{instance.GetDevice(), instance.GetAllocator()};
        new_backing->image.Create(new_image_ci);

        Vulkan::SetObjectName(instance.GetDevice(), new_backing->image.image,
                              "Image {}x{}x{} {} {} {:#x}:{:#x} L:{} M:{} S:{} (backing)",
                              info.size.width, info.size.height, info.size.depth,
                              AmdGpu::NameOf(info.tile_mode), vk::to_string(info.pixel_format),
                              info.guest_address, info.guest_size, info.resources.layers,
                              info.resources.levels, num_samples);
    } else {
        new_backing = std::addressof(*it);
    }

    if (copy_backing) {
        scheduler.EndRendering();
        ASSERT(image->info.resources.levels == 1 && image->info.resources.layers == 1);

        // Transition current backing to shader read layout
        Transit(image, vk::ImageLayout::eShaderReadOnlyOptimal,
                vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderRead);

        // Transition dest backing to color attachment layout, not caring of previous contents
        constexpr auto dst_stage = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
        constexpr auto dst_access =
            vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite;
        constexpr auto dst_layout = vk::ImageLayout::eColorAttachmentOptimal;
        image_barriers.push_back(vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = dst_stage,
            .dstAccessMask = dst_access,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = dst_layout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = new_backing->image,
            .subresourceRange{
                .aspectMask = image->aspect_mask,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = image->info.resources.layers,
            },
        });
        FlushBarriers();

        // Copy between ms and non ms backing images
        blit_helper->CopyBetweenMsImages(
            info.size.width, info.size.height, new_backing->num_samples, info.pixel_format,
            backing->num_samples > 1, backing->image, new_backing->image);

        // Update current layout in tracker to new backings layout
        new_backing->state.layout = dst_layout;
        new_backing->state.access_mask = dst_access;
        new_backing->state.pl_stage = dst_stage;
    }

    backing = new_backing;
}

bool Runtime::IsBufferAccessed(const VideoCore::Buffer* handle, u64 offset, u64 size,
                               bool check_read_access) {
    const AddressRange range = {
        .resource = reinterpret_cast<u64>(handle),
        .range_start = offset,
        .range_end = offset + size - 1,
    };
    bool has_access = barrier_tracker.FindRange(range, Access::Write);
    if (check_read_access && !has_access) {
        has_access |= barrier_tracker.FindRange(range, Access::Read);
    }
    return has_access;
}

void Runtime::AccessBuffer(const VideoCore::Buffer* handle, u64 offset, u64 size,
                           vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access) {
    AddressRange range = {
        .resource = reinterpret_cast<u64>(handle),
        .range_start = offset,
        .range_end = offset + size - 1,
    };

    constexpr static vk::AccessFlags2 READ_MASK =
        vk::AccessFlagBits2::eIndexRead | vk::AccessFlagBits2::eVertexAttributeRead |
        vk::AccessFlagBits2::eUniformRead | vk::AccessFlagBits2::eShaderRead |
        vk::AccessFlagBits2::eColorAttachmentRead |
        vk::AccessFlagBits2::eDepthStencilAttachmentRead | vk::AccessFlagBits2::eTransferRead |
        vk::AccessFlagBits2::eMemoryRead;

    constexpr static vk::AccessFlags2 WRITE_MASK =
        vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eColorAttachmentWrite |
        vk::AccessFlagBits2::eDepthStencilAttachmentWrite | vk::AccessFlagBits2::eTransferWrite |
        vk::AccessFlagBits2::eMemoryWrite | vk::AccessFlagBits2::eTransformFeedbackWriteEXT;

    // bbport: reads are tracked at 4 KiB granularity. Constant data comes from ring allocations
    // at a new offset every draw; rounded, they land in ranges already present and the
    // insert returns early instead of growing the tree until the next barrier flush.
    // Wider read ranges only add barriers, never drop one.
    if (!(src_access & WRITE_MASK) && !BbToggle::Disabled(BbToggle::CoarseReadTracking)) {
        range.range_start &= ~u64{0xFFF};
        range.range_end |= 0xFFF;
    }
    const auto insert = [&](Access access) {
        if (BbToggle::Disabled(BbToggle::AccessMemo)) {
            barrier_tracker.InsertRange(range, access);
            return;
        }
        u64 hash = range.resource ^ range.range_start * 0x9E3779B97F4A7C15ull ^
                   range.range_end << 13 ^ u64(access) << 61;
        hash = (hash ^ hash >> 31) * 0xFF51AFD7ED558CCDull;
        auto& memo = access_memo[(hash ^ hash >> 29) % access_memo.size()];
        if (memo.epoch == access_epoch && memo.resource == range.resource &&
            memo.start == range.range_start && memo.end == range.range_end &&
            memo.access == u32(access)) {
            return;
        }
        barrier_tracker.InsertRange(range, access);
        memo = {range.resource, range.range_start, range.range_end, access_epoch, u32(access)};
    };
    if (src_access & WRITE_MASK) {
        insert(Access::Write);
    }
    if (src_access & READ_MASK) {
        insert(Access::Read);
    }

    memory_barrier.srcStageMask |= src_stage;
    memory_barrier.srcAccessMask |= src_access & WRITE_MASK;
}

void Runtime::FlushBarriers() {
    BeforeImageAccess();
    vk::DependencyInfo dep_info{};

    if (memory_barrier.srcStageMask) {
        dep_info.pMemoryBarriers = &memory_barrier;
        dep_info.memoryBarrierCount = 1U;
    }
    if (!image_barriers.empty()) {
        dep_info.pImageMemoryBarriers = image_barriers.data();
        dep_info.imageMemoryBarrierCount = static_cast<u32>(image_barriers.size());
    }

    if (!dep_info.memoryBarrierCount && !dep_info.imageMemoryBarrierCount) {
        return;
    }

    scheduler.EndRendering();

    // Repeated area transitions exposed a crash inside the NVIDIA driver while an image
    // pipelineBarrier2 was being emitted by the deferred VkRecorder thread. Upstream shadPS4
    // records this barrier synchronously. Keep the optimized path by default, but Detailed Logs
    // can force only image barriers onto the caller thread to isolate/avoid that lifetime/race
    // without disabling threaded recording for every Vulkan command.
    static const bool direct_image_barriers = [] {
        const char* value = std::getenv("BB_DIRECT_IMAGE_BARRIERS");
        return value && value[0] == '1';
    }();

    if (direct_image_barriers && dep_info.imageMemoryBarrierCount != 0) {
        scheduler.CommandBuffer().pipelineBarrier2(dep_info);
    } else {
        scheduler.Record([memory = memory_barrier, has_memory = dep_info.memoryBarrierCount != 0,
                          images = scheduler.RecordData(std::span<const vk::ImageMemoryBarrier2>(
                              image_barriers.data(), image_barriers.size()))](vk::CommandBuffer cmdbuf) {
            const vk::DependencyInfo info = {
                .memoryBarrierCount = has_memory ? 1U : 0U,
                .pMemoryBarriers = has_memory ? &memory : nullptr,
                .imageMemoryBarrierCount = static_cast<u32>(images.size()),
                .pImageMemoryBarriers = images.data(),
            };
            cmdbuf.pipelineBarrier2(info);
        });
    }

    memory_barrier.srcStageMask = vk::PipelineStageFlagBits2::eNone;
    memory_barrier.srcAccessMask = vk::AccessFlagBits2::eNone;

    image_barriers.clear();
    barrier_tracker.Clear();
    ++access_epoch;
}

} // namespace Vulkan
