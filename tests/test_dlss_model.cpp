// SPDX-License-Identifier: GPL-2.0-or-later
// Real NGX/Vulkan recreation with synthetic frames; no game or save required.
#include <cstdio>
#include <vector>
#include "bbport_settings.h"
#include "video_core/renderer_vulkan/vk_dlss.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include <vk_mem_alloc.h>
#include "test_assert.h"
#ifdef _WIN32
#include "../src/crash_win.h"
static LONG WINAPI TestFault(EXCEPTION_POINTERS* e) {
    HMODULE module{}; char path[1024]{};
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<const char*>(e->ExceptionRecord->ExceptionAddress),&module);
    GetModuleFileNameA(module,path,sizeof(path));
    std::fprintf(stderr,"DLSS test fault: %08lx %s+%llx\n",e->ExceptionRecord->ExceptionCode,path,
        (unsigned long long)(e->ContextRecord->Rip-reinterpret_cast<uintptr_t>(module)));
    crash_win_dump(e);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

struct Image {
    const Vulkan::Instance& gpu;
    VkImage image{}; VkImageView view{}; VmaAllocation allocation{};
    vk::Format format; u32 width,height; VkImageAspectFlags aspect;
    Image(const Vulkan::Instance& g,u32 w,u32 h,vk::Format f,bool depth=false)
        :gpu(g),format(f),width(w),height(h),aspect(depth?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT) {
        VkImageCreateInfo ci{.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,
            .format=VkFormat(f),.extent={w,h,1},.mipLevels=1,.arrayLayers=1,
            .samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,
            .usage=VkImageUsageFlags(VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|
                (depth?VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT:VK_IMAGE_USAGE_STORAGE_BIT))};
        VmaAllocationCreateInfo ai{.usage=VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        assert(vmaCreateImage(gpu.GetAllocator(),&ci,&ai,&image,&allocation,nullptr)==VK_SUCCESS);
        VkImageViewCreateInfo vi{.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=image,
            .viewType=VK_IMAGE_VIEW_TYPE_2D,.format=VkFormat(f),.subresourceRange={aspect,0,1,0,1}};
        assert(VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateImageView(VkDevice(gpu.GetDevice()),&vi,nullptr,&view)==VK_SUCCESS);
    }
    ~Image(){VULKAN_HPP_DEFAULT_DISPATCHER.vkDestroyImageView(VkDevice(gpu.GetDevice()),view,nullptr);
        vmaDestroyImage(gpu.GetAllocator(),image,allocation);}
    Vulkan::Fsr4Upscaler::Image Resource() const {return {image,view,width,height,format};}
    void Clear(vk::CommandBuffer cb,float value) {
        vk::ImageMemoryBarrier barrier{.dstAccessMask=vk::AccessFlagBits::eTransferWrite,
            .oldLayout=vk::ImageLayout::eUndefined,.newLayout=vk::ImageLayout::eGeneral,
            .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
            .image=image,.subresourceRange={vk::ImageAspectFlags(aspect),0,1,0,1}};
        cb.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,vk::PipelineStageFlagBits::eTransfer,{}, {},{},barrier);
        if(aspect==VK_IMAGE_ASPECT_DEPTH_BIT) cb.clearDepthStencilImage(image,vk::ImageLayout::eGeneral,{value,0},barrier.subresourceRange);
        else cb.clearColorImage(image,vk::ImageLayout::eGeneral,vk::ClearColorValue{std::array{value,value,value,1.f}},barrier.subresourceRange);
        barrier.srcAccessMask=vk::AccessFlagBits::eTransferWrite;
        barrier.dstAccessMask=vk::AccessFlagBits::eShaderRead|vk::AccessFlagBits::eShaderWrite;
        barrier.oldLayout=barrier.newLayout;
        cb.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,vk::PipelineStageFlagBits::eComputeShader,{}, {},{},barrier);
    }
};
int main() {
    setvbuf(stdout,nullptr,_IONBF,0);
#ifdef _WIN32
    crash_win_init();SetUnhandledExceptionFilter(TestFault);
#endif
    Vulkan::Instance instance(0,false);
    static vk::detail::DynamicLoader loader;
    VULKAN_HPP_DEFAULT_DISPATCHER.init(loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr"));
    VULKAN_HPP_DEFAULT_DISPATCHER.init(instance.GetInstance());
    VULKAN_HPP_DEFAULT_DISPATCHER.init(instance.GetDevice());
    Vulkan::Scheduler scheduler(instance,true);
    Vulkan::DlssUpscaler dlss(instance,scheduler);
    if(!dlss.Available()){std::printf("SKIP: real DLSS test: %s\n",dlss.Problem());return 77;}
    const u32 ow=1280,oh=720;
    VmaTotalStatistics before{};
    vmaCalculateStatistics(instance.GetAllocator(),&before);
    for(int model:BbSettings::DlssPresets) for(int quality=0;quality<BbSettings::PresetCount;++quality) {
        VmaTotalStatistics live{};vmaCalculateStatistics(instance.GetAllocator(),&live);
        assert(live.total.statistics.allocationCount==before.total.statistics.allocationCount);
        BbSettings::Get().dlss_preset=model;
        dlss.OnFrameStart(true); // Matches the runtime boundary before scene buffers.
        auto scale=BbSettings::PresetScale(quality);
        const u32 w=u32(ow/scale/2)*2,h=u32(oh/scale/2)*2;
        Image color(instance,w,h,vk::Format::eR16G16B16A16Sfloat);
        Image depth(instance,w,h,vk::Format::eD32Sfloat,true);
        Image motion(instance,w,h,vk::Format::eR16G16Sfloat);
        Image output(instance,ow,oh,vk::Format::eR16G16B16A16Sfloat);
        auto cb=scheduler.CommandBuffer();
        color.Clear(cb,.25f);depth.Clear(cb,.5f);motion.Clear(cb,0);output.Clear(cb,0);
        Vulkan::Fsr4Upscaler::Frame frame{.cmdbuf=cb,.color=color.Resource(),.depth=depth.Resource(),
            .motion=motion.Resource(),.output=output.Resource(),.render_width=w,.render_height=h,
            .preset=quality,.jitter={0,0},.frame_ms=16.667f,.reset=false};
        assert(dlss.Record(frame)); scheduler.Finish();
        // Ensure a real evaluation wrote finite, nonblack output.
        VkBuffer buffer{};VmaAllocation memory{};VmaAllocationInfo map{};
        VkBufferCreateInfo bi{.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=ow*oh*8u,.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai{.flags=VMA_ALLOCATION_CREATE_MAPPED_BIT|VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,.usage=VMA_MEMORY_USAGE_AUTO};
        assert(vmaCreateBuffer(instance.GetAllocator(),&bi,&ai,&buffer,&memory,&map)==VK_SUCCESS);
        cb=scheduler.CommandBuffer();
        vk::ImageMemoryBarrier barrier{.srcAccessMask=vk::AccessFlagBits::eShaderWrite,.dstAccessMask=vk::AccessFlagBits::eTransferRead,
            .oldLayout=vk::ImageLayout::eGeneral,.newLayout=vk::ImageLayout::eGeneral,
            .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
            .image=output.image,.subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1}};
        cb.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,vk::PipelineStageFlagBits::eTransfer,{}, {},{},barrier);
        vk::BufferImageCopy copy{.imageSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},.imageExtent={ow,oh,1}};
        cb.copyImageToBuffer(output.image,vk::ImageLayout::eGeneral,buffer,copy);
        scheduler.Finish();vmaInvalidateAllocation(instance.GetAllocator(),memory,0,VK_WHOLE_SIZE);
        auto* pixels=static_cast<const u16*>(map.pMappedData);unsigned nonzero=0;
        for(u32 i=0;i<ow*oh;i+=31){u16 value=pixels[i*4];assert((value&0x7c00)!=0x7c00);nonzero+=value!=0;}
        assert(nonzero);vmaDestroyBuffer(instance.GetAllocator(),buffer,memory);
        std::printf("PASS: real NGX model=%s quality=%s %ux%u -> %ux%u, finite output\n",
            BbSettings::DlssPresetName(model),BbSettings::PresetName(quality),w,h,ow,oh);
    }
    // Restore Auto after a nondefault model without carrying its parameters over.
    BbSettings::Get().dlss_preset=0;assert(dlss.OnFrameStart(false));
    VmaTotalStatistics after{};vmaCalculateStatistics(instance.GetAllocator(),&after);
    assert(after.total.statistics.allocationCount==before.total.statistics.allocationCount);
    std::printf("PASS: no retained VMA image allocations after 55 NGX recreations (count=%u)\n",
        after.total.statistics.allocationCount);
    std::puts("PASS: returned to Auto, beginning normal resource shutdown");
}
