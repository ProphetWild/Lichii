// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// NVIDIA DLSS through NVIDIA Streamline. Only Streamline's MIT-licensed
// headers are built in: NVIDIA's DLLs are loaded at run time from the user's PC, and none ship
// with the emulator.

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "common/common_types.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan::Streamline {

/// NVIDIA's files DLSS needs, as found on this PC. Paths are empty when missing.
struct DlssFiles {
    std::filesystem::path folder;     ///< Where the loader is, or where it should go
    std::filesystem::path interposer; ///< sl.interposer.dll (Streamline's loader)
    std::filesystem::path model;      ///< nvngx_dlss.dll next to the loader
    std::filesystem::path app_model;  ///< Newest DLSS the NVIDIA app downloaded
    std::string interposer_version;
    std::string model_version;
    std::string app_model_version;
};

[[nodiscard]] DlssFiles FindDlssFiles();

/// The user folder's "streamline" folder.
[[nodiscard]] std::filesystem::path UserFolder();

/// Streamline's own modules carry a second, Streamline-only signature.
[[nodiscard]] bool IsStreamlineModule(const std::filesystem::path& path);

/// A valid Authenticode signature from NVIDIA Corporation (DLSS models).
[[nodiscard]] bool IsSignedByNvidia(const std::filesystem::path& path);

/// When the NVIDIA DLSS setting is on, loads the user's sl.interposer.dll (checking NVIDIA's
/// signature) and starts Streamline for DLSS. Call before creating the renderer's instance.
bool Start();

/// Whether Streamline is running and waiting for the renderer's device.
[[nodiscard]] bool WantsDevice();

/// Instance and device extensions DLSS asks for (empty when Streamline isn't running).
[[nodiscard]] std::vector<std::string> InstanceExtensions();
[[nodiscard]] std::vector<std::string> DeviceExtensions();

/// Hands the renderer's device to Streamline and routes the swapchain calls through it, as
/// Streamline's manual hooking needs. Stops Streamline and returns false when DLSS can't run.
/// Streamline's proxy for a hooked Vulkan function (sl_hooks.h) once frame generation has the
/// device, else null. Frame generation finds a swapchain's window through the surface proxies,
/// which only work after AttachDevice.
[[nodiscard]] PFN_vkVoidFunction Proxy(const char* name);

/// Queues frame generation wants in the graphics family after the host's first one.
[[nodiscard]] u32 ExtraGraphicsFamilyQueues();

/// `graphics_queues`: how many queues the device created in `graphics_family`.
bool AttachDevice(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                  u32 graphics_family, u32 graphics_queues, vk::DeviceDispatch& dld);

/// NVIDIA DLSS Frame Generation is on and Streamline took the device for it.
[[nodiscard]] bool IsFrameGenerationAvailable();

/// The GPU thread, as DLSS upscales a frame: its Streamline frame index. The next presented frame
/// shows it (TakeUpscaledFrame at composition).
void NoteUpscaledFrame(u32 index);
[[nodiscard]] std::optional<u32> TakeUpscaledFrame();

/// The presenting thread, around vkQueuePresentKHR. `frame` is the DLSS frame the image shows;
/// frames without one are presented with frame generation off.
void BeginPresent(std::optional<u32> frame);
void EndPresent(std::optional<u32> frame);

/// Whether DLSS can run on the attached device.
[[nodiscard]] bool IsDlssAvailable();

/// Shuts Streamline down; call before the device is destroyed.
void Stop();

} // namespace Vulkan::Streamline
