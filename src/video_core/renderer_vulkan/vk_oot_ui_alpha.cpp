// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdlib>
#include <utility>

#include "common/logging.h"
#include "video_core/host_shaders/oot_ui_alpha_comp_spv.h"
#include "video_core/renderer_vulkan/present/util.h"
#include "video_core/renderer_vulkan/vk_oot_ui_alpha.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/vulkan_common/streamline.h"
#include "video_core/vulkan_common/streamline_api.h"
#include "video_core/vulkan_common/vulkan_device.h"

namespace Vulkan {
namespace {

// Streamline has no R8_UNORM in its Vulkan format table.
constexpr VkFormat MaskFormat = VK_FORMAT_R16_SFLOAT;
constexpr VkImageUsageFlags MaskUsage =
    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

struct PushConstants {
    std::array<s32, 2> mask_size;
    std::array<s32, 2> screen_offset;
    std::array<s32, 2> screen_size;
    std::array<s32, 2> source_size;
};

constexpr VkImageSubresourceRange ColorRange{
    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
    .baseMipLevel = 0,
    .levelCount = 1,
    .baseArrayLayer = 0,
    .layerCount = 1,
};

vk::Image CreateImage(MemoryAllocator& allocator, VkExtent2D extent, VkFormat format,
                      VkImageUsageFlags usage) {
    return allocator.CreateImage(VkImageCreateInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = {extent.width, extent.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    });
}

vk::ImageView CreateView(const Device& device, VkImage image, VkFormat format) {
    return device.GetLogical().CreateImageView(VkImageViewCreateInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .image = image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .components{},
        .subresourceRange = ColorRange,
    });
}

void MemoryBarrier(vk::CommandBuffer& cmdbuf, VkAccessFlags src, VkAccessFlags dst) {
    const VkMemoryBarrier barrier{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = src,
        .dstAccessMask = dst,
    };
    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                           0, barrier);
}

} // Anonymous namespace

OotUiAlpha::OotUiAlpha(const Device& device_, MemoryAllocator& allocator_)
    : device{device_}, allocator{allocator_} {
    descriptor_pool = CreateWrappedDescriptorPool(
        device, 3 * SetCount, SetCount,
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE});
    descriptor_set_layout = CreateWrappedDescriptorSetLayout(
        device,
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
        VK_SHADER_STAGE_COMPUTE_BIT);
    const std::array layouts{*descriptor_set_layout, *descriptor_set_layout,
                             *descriptor_set_layout, *descriptor_set_layout};
    descriptor_sets = CreateWrappedDescriptorSets(descriptor_pool, layouts);
    const VkPushConstantRange range{
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size = sizeof(PushConstants),
    };
    pipeline_layout = device.GetLogical().CreatePipelineLayout(VkPipelineLayoutCreateInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .setLayoutCount = 1,
        .pSetLayouts = descriptor_set_layout.address(),
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &range,
    });
    shader = CreateWrappedShaderModule(device, OOT_UI_ALPHA_COMP_SPV);
    pipeline = CreateWrappedComputePipeline(device, pipeline_layout, *shader);
    sampler = CreateWrappedSampler(device, VK_FILTER_NEAREST);
}

OotUiAlpha::~OotUiAlpha() = default;

bool OotUiAlpha::Wanted() {
    static const bool disabled = [] {
        const char* value = std::getenv("EDEN_OOT_UI_ALPHA");
        return value != nullptr && value[0] == '0';
    }();
    return !disabled && Streamline::IsFrameGenerationAvailable();
}

void OotUiAlpha::CreateImages(VkFormat new_format, VkExtent2D new_extent) {
    snapshot = CreateImage(allocator, new_extent, new_format,
                           VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    snapshot_view = CreateView(device, *snapshot, new_format);
    mask = CreateImage(allocator, new_extent, MaskFormat, MaskUsage);
    mask_view = CreateView(device, *mask, MaskFormat);
    format = new_format;
    extent = new_extent;
    images_initialized = false;
}

void OotUiAlpha::Snapshot(Scheduler& scheduler, VkImage image, VkFormat image_format,
                          VkExtent2D image_extent) {
    if (!snapshot || format != image_format || extent.width != image_extent.width ||
        extent.height != image_extent.height) {
        if (snapshot) {
            scheduler.Finish(); // the old images may still be in use
        }
        CreateImages(image_format, image_extent);
    }
    const bool initialize = !std::exchange(images_initialized, true);
    const VkImage snapshot_image = *snapshot;
    const VkImage mask_image = *mask;
    const VkExtent2D size = extent;
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([=](vk::CommandBuffer cmdbuf) {
        if (initialize) {
            TransitionImageLayout(cmdbuf, snapshot_image, VK_IMAGE_LAYOUT_GENERAL,
                                  VK_IMAGE_LAYOUT_UNDEFINED);
            TransitionImageLayout(cmdbuf, mask_image, VK_IMAGE_LAYOUT_GENERAL,
                                  VK_IMAGE_LAYOUT_UNDEFINED);
        }
        MemoryBarrier(cmdbuf, VK_ACCESS_MEMORY_WRITE_BIT,
                      VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
        const VkImageCopy copy{
            .srcSubresource{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .srcOffset{},
            .dstSubresource{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .dstOffset{},
            .extent{size.width, size.height, 1},
        };
        cmdbuf.CopyImage(image, VK_IMAGE_LAYOUT_GENERAL, snapshot_image, VK_IMAGE_LAYOUT_GENERAL,
                         copy);
        MemoryBarrier(cmdbuf, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_MEMORY_READ_BIT);
    });
    snapshot_source = image;
    snapshot_pending = true;
}

void OotUiAlpha::Tag(Scheduler& scheduler, VkImage image, u32 frame_index) {
    if (!std::exchange(snapshot_pending, false) || image != snapshot_source) {
        return;
    }
    const u32 set_index = next_set;
    next_set = (next_set + 1) % SetCount;
    scheduler.Wait(set_ticks[set_index]);
    source_views[set_index] = CreateView(device, image, format);
    const VkDescriptorSet set = descriptor_sets[set_index];
    const VkDescriptorImageInfo with_ui{
        .sampler = *sampler,
        .imageView = *source_views[set_index],
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
    };
    const VkDescriptorImageInfo without_ui{
        .sampler = *sampler,
        .imageView = *snapshot_view,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
    };
    const VkDescriptorImageInfo mask_info{
        .sampler = VK_NULL_HANDLE,
        .imageView = *mask_view,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
    };
    const auto write = [&](u32 binding, VkDescriptorType type, const VkDescriptorImageInfo* info) {
        return VkWriteDescriptorSet{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .pNext = nullptr,
            .dstSet = set,
            .dstBinding = binding,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = type,
            .pImageInfo = info,
            .pBufferInfo = nullptr,
            .pTexelBufferView = nullptr,
        };
    };
    const std::array writes{
        write(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &with_ui),
        write(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &without_ui),
        write(2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &mask_info),
    };
    device.GetLogical().UpdateDescriptorSets(writes, {});

    const s32 width = static_cast<s32>(extent.width);
    const s32 height = static_cast<s32>(extent.height);
    const PushConstants constants{
        .mask_size = {width, height},
        .screen_offset = {0, 0},
        .screen_size = {width, height},
        .source_size = {width, height},
    };
    const VkPipeline compute_pipeline = *pipeline;
    const VkPipelineLayout layout = *pipeline_layout;
    const VkImage mask_image = *mask;
    const VkImageView mask_image_view = *mask_view;
    const VkExtent2D size = extent;
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([=](vk::CommandBuffer cmdbuf) {
        MemoryBarrier(cmdbuf, VK_ACCESS_MEMORY_WRITE_BIT,
                      VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, compute_pipeline);
        cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, set, {});
        cmdbuf.PushConstants(layout, VK_SHADER_STAGE_COMPUTE_BIT, constants);
        cmdbuf.Dispatch((size.width + 7) / 8, (size.height + 7) / 8, 1);
        MemoryBarrier(cmdbuf, VK_ACCESS_SHADER_WRITE_BIT,
                      VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_MEMORY_READ_BIT);

        // Streamline copies the mask now (it is reused next frame).
        const Streamline::Api& api = Streamline::GetApi();
        sl::FrameToken* token{};
        if (api.get_new_frame_token(token, &frame_index) != sl::Result::eOk || !token) {
            return;
        }
        sl::Resource resource{sl::ResourceType::eTex2d, mask_image, nullptr, mask_image_view,
                              static_cast<uint32_t>(VK_IMAGE_LAYOUT_GENERAL)};
        resource.width = size.width;
        resource.height = size.height;
        resource.nativeFormat = static_cast<uint32_t>(MaskFormat);
        resource.mipLevels = 1;
        resource.arrayLayers = 1;
        resource.flags = 0;
        resource.usage = MaskUsage;
        const sl::Extent area{0, 0, size.width, size.height};
        const sl::ResourceTag tag{&resource, sl::kBufferTypeUIAlpha,
                                  sl::ResourceLifecycle::eOnlyValidNow, &area};
        const sl::ViewportHandle viewport{0u};
        if (const sl::Result result = api.set_tag_for_frame(
                *token, viewport, &tag, 1, reinterpret_cast<sl::CommandBuffer*>(*cmdbuf));
            result != sl::Result::eOk) {
            static bool logged{};
            if (!std::exchange(logged, true)) {
                LOG_WARNING(Render_Vulkan, "Tagging the OoT UI alpha failed (Streamline result {})",
                            static_cast<int>(result));
            }
        }
    });
    set_ticks[set_index] = scheduler.CurrentTick();
    if (++tagged == 1) {
        LOG_INFO(Render_Vulkan, "OoT DLSS Frame Generation: tagging the HUD as UI alpha ({}x{})",
                 extent.width, extent.height);
    }
}

} // namespace Vulkan
