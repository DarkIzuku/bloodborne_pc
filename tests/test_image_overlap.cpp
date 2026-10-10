// SPDX-License-Identifier: GPL-2.0-or-later
// Regression cases for real ImageInfo coverage/mip/slice selection; no Vulkan device.
#include <cassert>
#include <limits>
#include "video_core/amdgpu/resource.h"
#include "video_core/texture_cache/image_info.h"
#include "video_core/renderer_vulkan/image_copy_region.h"

static VideoCore::ImageInfo Image(u32 mips, u32 layers) {
    VideoCore::ImageInfo image{};
    image.type = AmdGpu::ImageType::Color2D;
    image.pixel_format = vk::Format::eR8G8B8A8Unorm;
    image.size = {128, 128, 1};
    image.pitch = 128;
    image.num_bits = 32;
    image.resources = {mips, layers};
    image.guest_address = 0x100000;
    for (u32 m = 0; m < mips; ++m) {
        const u32 width = 128 >> m;
        const u32 bytes = width * width * 4 * layers;
        image.mips_layout[m] = {bytes, width, width, image.guest_size};
        image.guest_size += bytes;
    }
    return image;
}

int main() {
    using namespace VideoCore;
    const SubresourceExtent mip_heavy{4, 1}, layer_heavy{1, 6};
    assert(!mip_heavy.Contains(layer_heavy)); // Lexicographical > used to accept this.
    assert(!layer_heavy.Contains(mip_heavy));
    assert(!mip_heavy.Contains({1, 1}, {std::numeric_limits<u32>::max(), 0}));
    assert(!mip_heavy.Contains({0, 1}));

    const auto parent = Image(4, 6);
    auto requested = Image(1, 6);
    auto candidate = Image(4, 1);
    candidate.guest_size = requested.guest_size; // Equal allocation bytes are not coverage.
    assert(!requested.ViewRejection(candidate).empty());
    assert(!Image(4, 1).ViewRejection(Image(1, 6)).empty());
    assert(requested.ViewRejection(parent).empty());

    // Two adjacent layers starting at an ODD parent slice. Old code divided/modded by the
    // whole two-layer requested size, rejecting this valid view or returning the wrong slice.
    auto child = Image(1, 2);
    child.size = {32, 32, 1};
    child.pitch = 32;
    const u32 slice_bytes = parent.mips_layout[2].size / parent.resources.layers;
    child.guest_address = parent.guest_address + parent.mips_layout[2].offset + slice_bytes;
    child.guest_size = slice_bytes * 2;
    child.mips_layout[0] = {child.guest_size, 32, 32, 0};
    assert(child.MipOf(parent) == 2);
    assert(child.SliceOf(parent, 2) == 1);
    assert(child.ViewRejection(parent, {2, 1}).empty());
    child.guest_address += slice_bytes;
    assert(child.SliceOf(parent, 2) == 2); // Previously returned 1.
    child.guest_address = parent.guest_address + parent.mips_layout[2].offset + slice_bytes * 5;
    assert(child.SliceOf(parent, 2) == -1); // Last slice cannot contain TWO layers.
    assert(!child.ViewRejection(parent, {2, 5}).empty());
    child.guest_address = parent.guest_address - 1;
    assert(child.SliceOf(parent, 2) == -1); // No unsigned address underflow.
    assert(child.SliceOf(parent, -1) == -1);
    assert(child.SliceOf(parent, 16) == -1);

    requested = Image(4, 3); // Parent has 6 layers: later mip addresses/strides differ.
    assert(!requested.ViewRejection(parent).empty());
    requested = Image(4, 6);
    requested.pixel_format = vk::Format::eR8G8B8A8Srgb;
    assert(requested.IsCompatible(parent));
    assert(requested.ViewRejection(parent).empty());
    requested.num_samples = 4;
    assert(!requested.ViewRejection(parent).empty());
    requested = parent;
    requested.type = AmdGpu::ImageType::Color3D;
    assert(!requested.ViewRejection(parent).empty());
    requested = parent;
    requested.size.width *= 2;
    assert(!requested.ViewRejection(parent).empty());
    requested = parent;
    ++requested.guest_size;
    assert(!requested.ViewRejection(parent).empty());
    requested = parent;
    ++requested.bank_swizzle;
    // Attachment descriptions omit this field; it must not invalidate a contained view.
    assert(requested.ViewRejection(parent).empty());
    assert(requested.LayoutKey() != parent.LayoutKey());
    requested.alt_tile = !parent.alt_tile;
    assert(!requested.ViewRejection(parent).empty());
    auto padded = Image(1, 1);
    auto unpadded = padded;
    padded.props.is_pow2 = 1;
    padded.guest_size *= 2;
    assert(padded.PaddingOnlyDifference(unpadded));
    assert(padded.ViewRejection(unpadded).empty());
    // Tiling can pad the physical mip pitch beyond the descriptor's logical pitch.
    auto narrow = Image(1, 1);
    narrow.pitch = narrow.size.width = 100;
    assert(narrow.ViewRejection(narrow).empty());
    auto oversized_slice = Image(1, 1);
    oversized_slice.guest_size *= 2;
    oversized_slice.mips_layout[0].size *= 2;
    assert(!oversized_slice.ViewRejection(Image(1, 6)).empty());

    using Vulkan::PlanCopyLayers;
    auto copy = PlanCopyLayers(false, false, 2, 6, 1, 1, 0, 3);
    assert(copy.source == 2 && copy.destination == 2 && copy.depth == 1);
    copy = PlanCopyLayers(false, false, 2, 6, 1, 1, 0, 5);
    assert(copy.source == 1 && copy.destination == 1); // Do not overrun dst base + count.
    copy = PlanCopyLayers(false, false, 8, 8, 1, 1, 2, 3, 2);
    assert(copy.source == 2 && copy.destination == 2); // Selected views limit the copy.
    copy = PlanCopyLayers(false, true, 3, 1, 1, 8);
    assert(copy.source == 3 && copy.destination == 1 && copy.depth == 3);
    copy = PlanCopyLayers(true, false, 1, 4, 8, 1, 0, 3);
    assert(copy.source == 1 && copy.destination == 1 && copy.depth == 1);
    copy = PlanCopyLayers(true, true, 1, 1, 8, 4);
    assert(copy.source == 1 && copy.destination == 1 && copy.depth == 4);
    assert(!PlanCopyLayers(false, false, 2, 2, 1, 1, 0, 2));
    assert(!PlanCopyLayers(true, true, 1, 1, 8, 8, 1, 0));
    assert(!PlanCopyLayers(false, false, 1, 1, 1, 1, 0, 0, 0));
}
