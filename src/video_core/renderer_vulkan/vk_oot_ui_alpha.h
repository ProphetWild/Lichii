// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>

#include "common/common_types.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;
class Scheduler;

/// UI alpha for DLSS Frame Generation in Ocarina of Time (host_shaders/oot_ui_alpha.comp). The
/// game draws its HUD over the DLSS output image after DLSS: Snapshot copies that image before
/// the HUD, and Tag compares it with the finished image at composition and tags the difference to
/// Streamline as sl::kBufferTypeUIAlpha for that frame, so frame generation leaves the HUD alone.
/// EDEN_OOT_UI_ALPHA=0 turns it off.
class OotUiAlpha {
public:
    explicit OotUiAlpha(const Device& device, MemoryAllocator& allocator);
    ~OotUiAlpha();

    /// Frame generation is on and the mask is wanted.
    [[nodiscard]] static bool Wanted();

    /// Right after DLSS's output is drawn into `image` (GENERAL), before the HUD.
    void Snapshot(Scheduler& scheduler, VkImage image, VkFormat format, VkExtent2D extent);

    /// At the composition that shows Streamline frame `frame_index`: `image` is the output image
    /// as the game left it (with its HUD). Does nothing without a snapshot of that image.
    void Tag(Scheduler& scheduler, VkImage image, u32 frame_index);

private:
    static constexpr u32 SetCount = 4;

    void CreateImages(VkFormat format, VkExtent2D extent);

    const Device& device;
    MemoryAllocator& allocator;
    vk::DescriptorPool descriptor_pool;
    vk::DescriptorSetLayout descriptor_set_layout;
    vk::DescriptorSets descriptor_sets;
    std::array<u64, SetCount> set_ticks{};
    /// Views of the game's image, one per descriptor set (the image can change between frames).
    std::array<vk::ImageView, SetCount> source_views;
    u32 next_set{};
    vk::PipelineLayout pipeline_layout;
    vk::ShaderModule shader;
    vk::Pipeline pipeline;
    vk::Sampler sampler;

    vk::Image snapshot;
    vk::ImageView snapshot_view;
    vk::Image mask;
    vk::ImageView mask_view;
    VkFormat format{};
    VkExtent2D extent{};
    bool images_initialized{};
    VkImage snapshot_source{}; ///< The image the pending snapshot was taken of
    bool snapshot_pending{};
    u64 tagged{};
};

} // namespace Vulkan
