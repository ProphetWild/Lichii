// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// NVIDIA DLSS through NVIDIA Streamline, with Streamline's manual hooking.

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <fmt/format.h>

#include "common/fs/fs_util.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "video_core/vulkan_common/streamline.h"
#include "video_core/vulkan_common/streamline_api.h"

#include <sl_security.h>

#include <softpub.h>
#include <wintrust.h>

namespace Vulkan::Streamline {
namespace {

// A custom engine names itself with its own project id; an application id from NVIDIA is only
// needed without an engine type. Keep it fixed: the driver may key behaviour off it.
constexpr const char* PROJECT_ID = "8d3e2f9a-6b41-4c7e-9a52-1f0c3b7d5e64";
constexpr const wchar_t* INTERPOSER = L"sl.interposer.dll";
constexpr const wchar_t* MODEL = L"nvngx_dlss.dll";
// The first DLSS with preset K.
constexpr u32 MIN_DLSS_MAJOR = 310;

using Version = std::array<u32, 4>;

struct State {
    HMODULE module{};
    Api api{};
    std::wstring plugin_dir;
    std::wstring model_dir;
    std::vector<std::string> instance_extensions;
    std::vector<std::string> device_extensions;
    bool started{};
    bool attached{};
    /// Frame generation was asked for and its plugin loaded.
    bool frame_generation{};
    /// ...and Streamline took the device for it.
    bool frame_generation_attached{};
    u32 fg_graphics_queues{};
    u32 fg_compute_queues{};
    /// Presenting thread only.
    sl::DLSSGMode fg_mode = sl::DLSSGMode::eOff;
    u32 fg_frames{};
    u32 fg_frames_max{};
    bool fg_mode_set{};
    u64 fg_presents{};
};

/// The frame DLSS last upscaled, for the next composition (~0u: none since the last one).
std::atomic<u32> g_upscaled_frame{~0u};

State& GetState() {
    static State state;
    return state;
}

int LogLevel() {
    const char* const value = std::getenv("EDEN_NX2_DLSS_LOG");
    return value ? std::atoi(value) : 0;
}

void LogFromStreamline(sl::LogType type, const char* message) {
    std::string_view text{message ? message : ""};
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    switch (type) {
    case sl::LogType::eError:
        LOG_ERROR(Render_Vulkan, "Streamline: {}", text);
        break;
    case sl::LogType::eWarn:
        LOG_WARNING(Render_Vulkan, "Streamline: {}", text);
        break;
    default:
        if (LogLevel() > 0) {
            LOG_INFO(Render_Vulkan, "Streamline: {}", text);
        }
        break;
    }
}

/// Where the user may put sl.interposer.dll: EDEN_STREAMLINE_DIR, the user folder's "streamline"
/// folder, then a "streamline" folder next to the executable.
std::vector<std::filesystem::path> SearchDirs() {
    std::vector<std::filesystem::path> dirs;
    if (const char* const dir = std::getenv("EDEN_STREAMLINE_DIR"); dir && *dir) {
        dirs.emplace_back(dir);
    }
    dirs.push_back(UserFolder());
    dirs.push_back(Common::FS::GetExeDirectory() / "streamline");
    return dirs;
}

template <typename T>
bool Resolve(HMODULE module, const char* name, T*& function) {
    function = reinterpret_cast<T*>(GetProcAddress(module, name));
    if (!function) {
        LOG_ERROR(Render_Vulkan, "sl.interposer.dll has no {}", name);
    }
    return function != nullptr;
}

std::optional<Version> ReadVersion(const std::filesystem::path& path) {
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (size == 0) {
        return std::nullopt;
    }
    std::vector<u8> data(size);
    VS_FIXEDFILEINFO* info{};
    UINT length = 0;
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data()) ||
        !VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &length) || !info) {
        return std::nullopt;
    }
    return Version{HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
                   HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS)};
}

std::string VersionText(const std::filesystem::path& path) {
    const std::optional<Version> version = path.empty() ? std::nullopt : ReadVersion(path);
    if (!version) {
        return {};
    }
    const Version& v = *version;
    return v[3] == 0 ? fmt::format("{}.{}.{}", v[0], v[1], v[2])
                     : fmt::format("{}.{}.{}.{}", v[0], v[1], v[2], v[3]);
}

/// The NVIDIA app keeps every DLSS it downloads under ProgramData; pick the newest.
std::filesystem::path FindAppModel() {
    const wchar_t* const program_data = _wgetenv(L"ProgramData");
    if (!program_data) {
        return {};
    }
    const std::filesystem::path root =
        std::filesystem::path{program_data} / L"NVIDIA" / L"NGX" / L"models" / L"dlss" / L"versions";
    std::filesystem::path best_path;
    Version best{};
    std::error_code ec;
    for (const auto& version_dir : std::filesystem::directory_iterator(root, ec)) {
        std::error_code files_ec;
        for (const auto& file :
             std::filesystem::directory_iterator(version_dir.path() / L"files", files_ec)) {
            if (file.path().extension() != L".bin") {
                continue;
            }
            const std::optional<Version> version = ReadVersion(file.path());
            if (version && (*version)[0] >= MIN_DLSS_MAJOR && *version > best) {
                best = *version;
                best_path = file.path();
            }
        }
    }
    return best_path;
}

/// Copies the NVIDIA app's DLSS into our folder as nvngx_dlss.dll, the name Streamline loads.
std::filesystem::path UseAppModel(const DlssFiles& files) {
    const std::filesystem::path dir = UserFolder() / L"nvidia-app";
    const std::filesystem::path target = dir / MODEL;
    std::error_code ec;
    const bool current = std::filesystem::is_regular_file(target, ec) &&
                         std::filesystem::file_size(target, ec) ==
                             std::filesystem::file_size(files.app_model, ec) &&
                         ReadVersion(target) == ReadVersion(files.app_model);
    if (!current) {
        if (!IsSignedByNvidia(files.app_model)) {
            LOG_WARNING(Render_Vulkan, "The NVIDIA app's DLSS {} is not signed by NVIDIA",
                        Common::FS::PathToUTF8String(files.app_model));
            return {};
        }
        std::filesystem::create_directories(dir, ec);
        std::filesystem::copy_file(files.app_model, target,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            LOG_WARNING(Render_Vulkan, "Could not copy the NVIDIA app's DLSS to {}: {}",
                        Common::FS::PathToUTF8String(target), ec.message());
            return {};
        }
    }
    if (!IsSignedByNvidia(target)) {
        LOG_WARNING(Render_Vulkan, "Cached DLSS {} is not signed by NVIDIA",
                    Common::FS::PathToUTF8String(target));
        return {};
    }
    return dir;
}

bool Load(State& state, const std::filesystem::path& path) {
    if (!IsStreamlineModule(path)) {
        LOG_ERROR(Render_Vulkan, "{} is not signed by NVIDIA; not loading it",
                  Common::FS::PathToUTF8String(path));
        return false;
    }
    state.module = LoadLibraryW(path.c_str());
    if (!state.module) {
        LOG_ERROR(Render_Vulkan, "Could not load {} (error {})", Common::FS::PathToUTF8String(path),
                  GetLastError());
        return false;
    }
    Api& api = state.api;
    const HMODULE m = state.module;
    if (Resolve(m, "slInit", api.init) && Resolve(m, "slShutdown", api.shutdown) &&
        Resolve(m, "slIsFeatureSupported", api.is_feature_supported) &&
        Resolve(m, "slGetFeatureRequirements", api.get_feature_requirements) &&
        Resolve(m, "slGetFeatureVersion", api.get_feature_version) &&
        Resolve(m, "slGetFeatureFunction", api.get_feature_function) &&
        Resolve(m, "slGetNewFrameToken", api.get_new_frame_token) &&
        Resolve(m, "slSetConstants", api.set_constants) &&
        Resolve(m, "slEvaluateFeature", api.evaluate_feature) &&
        Resolve(m, "slFreeResources", api.free_resources) &&
        Resolve(m, "slSetVulkanInfo", api.set_vulkan_info) &&
        Resolve(m, "slSetTagForFrame", api.set_tag_for_frame)) {
        return true;
    }
    FreeLibrary(state.module);
    state.module = nullptr;
    state.api = {};
    return false;
}

std::vector<std::string> Names(uint32_t count, const char** names) {
    std::vector<std::string> result;
    for (uint32_t i = 0; i < count; ++i) {
        result.emplace_back(names[i]);
    }
    return result;
}

} // Anonymous namespace

std::filesystem::path UserFolder() {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::StreamlineDir);
}

bool IsStreamlineModule(const std::filesystem::path& path) {
    return sl::security::verifyEmbeddedSignature(path.c_str());
}

bool IsSignedByNvidia(const std::filesystem::path& path) {
    const std::wstring file_path = std::filesystem::absolute(path).wstring();
    WINTRUST_FILE_INFO file{};
    file.cbStruct = sizeof(file);
    file.pcwszFilePath = file_path.c_str();
    WINTRUST_DATA data{};
    data.cbStruct = sizeof(data);
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_NONE;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &file;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    bool valid = WinVerifyTrust(nullptr, &action, &data) == ERROR_SUCCESS;
    if (valid) {
        CRYPT_PROVIDER_DATA* const provider = WTHelperProvDataFromStateData(data.hWVTStateData);
        CRYPT_PROVIDER_SGNR* const signer =
            provider ? WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0) : nullptr;
        CRYPT_PROVIDER_CERT* const cert = signer ? WTHelperGetProvCertFromChain(signer, 0) : nullptr;
        wchar_t name[128]{};
        valid = cert &&
                CertGetNameStringW(cert->pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, name,
                                   static_cast<DWORD>(std::size(name))) > 1 &&
                std::wstring_view{name} == L"NVIDIA Corporation";
    }
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &action, &data);
    return valid;
}

DlssFiles FindDlssFiles() {
    DlssFiles files;
    files.folder = UserFolder();
    std::error_code ec;
    for (const std::filesystem::path& dir : SearchDirs()) {
        if (std::filesystem::is_regular_file(dir / INTERPOSER, ec)) {
            files.folder = dir;
            files.interposer = dir / INTERPOSER;
            break;
        }
    }
    if (std::filesystem::is_regular_file(files.folder / MODEL, ec)) {
        files.model = files.folder / MODEL;
    }
    files.app_model = FindAppModel();
    files.interposer_version = VersionText(files.interposer);
    files.model_version = VersionText(files.model);
    files.app_model_version = VersionText(files.app_model);
    return files;
}

bool Start() {
    State& state = GetState();
    if (state.started) {
        return true;
    }
    if (!Settings::values.nvidia_dlss.GetValue()) {
        return false;
    }
    if (!state.module) {
        const DlssFiles files = FindDlssFiles();
        if (!files.model.empty() && !IsSignedByNvidia(files.model)) {
            LOG_WARNING(Render_Vulkan, "DLSS model {} is not signed by NVIDIA; using FSR 3.1",
                        Common::FS::PathToUTF8String(files.model));
            return false;
        }
        if (files.interposer.empty()) {
            LOG_WARNING(Render_Vulkan,
                        "NVIDIA DLSS is on, but sl.interposer.dll (NVIDIA Streamline) is not in "
                        "{} (the Graphics settings can download it); using FSR 3.1",
                        Common::FS::PathToUTF8String(files.folder));
            return false;
        }
        if (!Load(state, files.interposer)) {
            return false;
        }
        state.plugin_dir = files.folder.wstring();
        state.model_dir.clear();
        if (files.model.empty() && !files.app_model.empty()) {
            if (const std::filesystem::path dir = UseAppModel(files); !dir.empty()) {
                state.model_dir = dir.wstring();
                LOG_INFO(Render_Vulkan, "Using the NVIDIA app's DLSS {}", files.app_model_version);
            }
        } else if (files.model.empty()) {
            LOG_WARNING(Render_Vulkan, "No nvngx_dlss.dll in {} and no DLSS from the NVIDIA app",
                        Common::FS::PathToUTF8String(files.folder));
        }
    }

    // Streamline's plugins come from the loader's folder or the NVIDIA app's downloads (OTA); the
    // DLSS model from the loader's folder, else the NVIDIA app's copy.
    const wchar_t* plugin_paths[] = {state.plugin_dir.c_str(), state.model_dir.c_str()};
    // PCL (latency markers) loads by default; frame generation also needs Reflex.
    // EDEN_DLSS_FG=0/1 overrides the setting (A/B tests).
    const bool want_fg = [] {
        if (const char* value = std::getenv("EDEN_DLSS_FG"); value != nullptr && *value != '\0') {
            return value[0] == '1';
        }
        return Settings::values.nvidia_dlss_frame_generation.GetValue();
    }();
    const sl::Feature features_dlss[] = {sl::kFeatureDLSS};
    const sl::Feature features_fg[] = {sl::kFeatureDLSS, sl::kFeatureDLSS_G,
                                       sl::kFeatureReflex};
    sl::Preferences preferences{};
    preferences.logLevel = LogLevel() >= 2 ? sl::LogLevel::eVerbose : sl::LogLevel::eDefault;
    preferences.logMessageCallback = LogFromStreamline;
    preferences.pathsToPlugins = plugin_paths;
    preferences.numPathsToPlugins = state.model_dir.empty() ? 1 : 2;
    preferences.flags = sl::PreferenceFlags::eDisableCLStateTracking |
                        sl::PreferenceFlags::eUseManualHooking | sl::PreferenceFlags::eAllowOTA |
                        sl::PreferenceFlags::eLoadDownloadedPlugins;
    if (want_fg) {
        // Frame generation's inputs are tagged per frame (slSetTagForFrame).
        preferences.flags |= sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    }
    preferences.featuresToLoad = want_fg ? features_fg : features_dlss;
    preferences.numFeaturesToLoad =
        static_cast<uint32_t>(want_fg ? std::size(features_fg) : std::size(features_dlss));
    preferences.engine = sl::EngineType::eCustom;
    preferences.engineVersion = "1.0";
    preferences.projectId = PROJECT_ID;
    preferences.renderAPI = sl::RenderAPI::eVulkan;
    const Api& api = state.api;
    if (const sl::Result result = api.init(preferences, sl::kSDKVersion);
        result != sl::Result::eOk) {
        LOG_ERROR(Render_Vulkan, "NVIDIA Streamline failed to start (result {}); using FSR 3.1",
                  static_cast<int>(result));
        return false;
    }
    sl::FeatureRequirements requirements{};
    if (const sl::Result result = api.get_feature_requirements(sl::kFeatureDLSS, requirements);
        result != sl::Result::eOk) {
        LOG_WARNING(Render_Vulkan,
                    "Streamline found no DLSS plugin (result {}); put sl.dlss.dll next to "
                    "sl.interposer.dll or install the NVIDIA app. Using FSR 3.1",
                    static_cast<int>(result));
        api.shutdown();
        return false;
    }
    if (requirements.vkNumComputeQueuesRequired != 0 ||
        requirements.vkNumGraphicsQueuesRequired != 0) {
        LOG_WARNING(Render_Vulkan, "This DLSS wants extra Vulkan queues; using FSR 3.1");
        api.shutdown();
        return false;
    }
    state.instance_extensions =
        Names(requirements.vkNumInstanceExtensions, requirements.vkInstanceExtensions);
    state.device_extensions =
        Names(requirements.vkNumDeviceExtensions, requirements.vkDeviceExtensions);
    // The KHR and EXT buffer device address extensions can't both be enabled.
    const auto& device_extensions = state.device_extensions;
    if (std::ranges::find(device_extensions, VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME) !=
        device_extensions.end()) {
        std::erase(state.device_extensions, VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
    }
    state.frame_generation = false;
    if (want_fg) {
        sl::FeatureRequirements fg{};
        if (const sl::Result result = api.get_feature_requirements(sl::kFeatureDLSS_G, fg);
            result != sl::Result::eOk) {
            LOG_WARNING(Render_Vulkan,
                        "DLSS Frame Generation is on, but Streamline found no sl.dlss_g plugin "
                        "(result {}); it needs sl.dlss_g.dll, sl.reflex.dll, sl.pcl.dll and "
                        "nvngx_dlssg.dll next to sl.interposer.dll",
                        static_cast<int>(result));
        } else {
            state.frame_generation = true;
            state.fg_graphics_queues = fg.vkNumGraphicsQueuesRequired;
            state.fg_compute_queues = fg.vkNumComputeQueuesRequired;
            const auto add = [](std::vector<std::string>& list, std::vector<std::string> names) {
                for (std::string& name : names) {
                    if (std::ranges::find(list, name) == list.end()) {
                        list.push_back(std::move(name));
                    }
                }
            };
            add(state.instance_extensions,
                Names(fg.vkNumInstanceExtensions, fg.vkInstanceExtensions));
            add(state.device_extensions, Names(fg.vkNumDeviceExtensions, fg.vkDeviceExtensions));
            std::string features;
            for (const std::string& name : Names(fg.vkNumFeatures12, fg.vkFeatures12)) {
                features += " " + name;
            }
            for (const std::string& name : Names(fg.vkNumFeatures13, fg.vkFeatures13)) {
                features += " " + name;
            }
            LOG_INFO(Render_Vulkan,
                     "DLSS Frame Generation wants {} graphics and {} compute queues and {} optical "
                     "flow queues (optical flow runs in interop mode); Vulkan features:{}",
                     fg.vkNumGraphicsQueuesRequired, fg.vkNumComputeQueuesRequired,
                     fg.vkNumOpticalFlowQueuesRequired, features.empty() ? " none" : features);
        }
    }
    sl::FeatureVersion version{};
    api.get_feature_version(sl::kFeatureDLSS, version);
    LOG_INFO(Render_Vulkan, "NVIDIA Streamline {} started from {} (DLSS {})",
             version.versionSL.toStr(),
             Common::FS::PathToUTF8String(std::filesystem::path{state.plugin_dir}),
             version.versionNGX.toStr());
    state.started = true;
    return true;
}

bool WantsDevice() {
    const State& state = GetState();
    return state.started && !state.attached;
}

std::vector<std::string> InstanceExtensions() {
    const State& state = GetState();
    return state.started ? state.instance_extensions : std::vector<std::string>{};
}

std::vector<std::string> DeviceExtensions() {
    const State& state = GetState();
    return state.started ? state.device_extensions : std::vector<std::string>{};
}

PFN_vkVoidFunction Proxy(const char* name) {
    const State& state = GetState();
    if (!IsFrameGenerationAvailable() || !state.module) {
        return nullptr;
    }
    return reinterpret_cast<PFN_vkVoidFunction>(GetProcAddress(state.module, name));
}

u32 ExtraGraphicsFamilyQueues() {
    const State& state = GetState();
    return state.started && state.frame_generation
               ? state.fg_graphics_queues + state.fg_compute_queues
               : 0;
}

bool AttachDevice(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                  u32 graphics_family, u32 graphics_queues, vk::DeviceDispatch& dld) {
    State& state = GetState();
    if (!WantsDevice()) {
        return false;
    }
    if (state.frame_generation && graphics_queues < 1 + ExtraGraphicsFamilyQueues()) {
        LOG_WARNING(Render_Vulkan,
                    "DLSS Frame Generation needs {} more queues in the graphics family; the device "
                    "has {}. Frame generation is off",
                    ExtraGraphicsFamilyQueues(), graphics_queues);
        state.frame_generation = false;
    }
    sl::VulkanInfo info{};
    info.device = device;
    info.instance = instance;
    info.physicalDevice = physical;
    info.computeQueueFamily = graphics_family;
    info.graphicsQueueFamily = graphics_family;
    // Streamline's own queues follow the host's queue 0 in the graphics family.
    if (state.frame_generation) {
        info.graphicsQueueIndex = 1;
        info.computeQueueIndex = 1 + state.fg_graphics_queues;
    }
    if (const sl::Result result = state.api.set_vulkan_info(info); result != sl::Result::eOk) {
        LOG_ERROR(Render_Vulkan, "Streamline did not take the Vulkan device (result {})",
                  static_cast<int>(result));
        Stop();
        return false;
    }
    sl::AdapterInfo adapter{};
    adapter.vkPhysicalDevice = physical;
    if (const sl::Result result = state.api.is_feature_supported(sl::kFeatureDLSS, adapter);
        result != sl::Result::eOk) {
        LOG_WARNING(Render_Vulkan,
                    "NVIDIA DLSS is not supported on this GPU or driver (Streamline result {}); "
                    "using FSR 3.1",
                    static_cast<int>(result));
        Stop();
        return false;
    }
    // Manual hooking: Streamline does its per-frame work in these.
    const auto hook = [&](auto& function, const char* name) {
        if (const FARPROC proxy = GetProcAddress(state.module, name)) {
            function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(proxy);
        } else {
            LOG_WARNING(Render_Vulkan, "sl.interposer.dll has no {}", name);
        }
    };
    hook(dld.vkCreateSwapchainKHR, "vkCreateSwapchainKHR");
    hook(dld.vkDestroySwapchainKHR, "vkDestroySwapchainKHR");
    hook(dld.vkGetSwapchainImagesKHR, "vkGetSwapchainImagesKHR");
    hook(dld.vkAcquireNextImageKHR, "vkAcquireNextImageKHR");
    hook(dld.vkQueuePresentKHR, "vkQueuePresentKHR");
    hook(dld.vkDeviceWaitIdle, "vkDeviceWaitIdle");
    state.attached = true;
    LOG_INFO(Render_Vulkan, "NVIDIA DLSS is available through Streamline");
    state.frame_generation_attached = false;
    if (state.frame_generation) {
        Api& api = state.api;
        const auto function = [&](sl::Feature feature, const char* name, auto& target) {
            void* pointer{};
            if (api.get_feature_function(feature, name, pointer) != sl::Result::eOk || !pointer) {
                LOG_WARNING(Render_Vulkan, "Streamline has no {}", name);
                return false;
            }
            target = reinterpret_cast<std::remove_reference_t<decltype(target)>>(pointer);
            return true;
        };
        if (const sl::Result result = api.is_feature_supported(sl::kFeatureDLSS_G, adapter);
            result != sl::Result::eOk) {
            LOG_WARNING(Render_Vulkan,
                        "DLSS Frame Generation is not supported here (Streamline result {}); it "
                        "needs a GeForce RTX 40 series or newer and hardware-accelerated GPU "
                        "scheduling",
                        static_cast<int>(result));
        } else if (function(sl::kFeatureDLSS_G, "slDLSSGSetOptions", api.dlssg_set_options) &&
                   function(sl::kFeatureDLSS_G, "slDLSSGGetState", api.dlssg_get_state) &&
                   function(sl::kFeatureReflex, "slReflexSetOptions", api.reflex_set_options) &&
                   function(sl::kFeaturePCL, "slPCLSetMarker", api.pcl_set_marker)) {
            sl::ReflexOptions reflex{};
            reflex.mode = sl::ReflexMode::eLowLatency;
            if (const sl::Result result = api.reflex_set_options(reflex);
                result != sl::Result::eOk) {
                LOG_WARNING(Render_Vulkan, "Reflex options failed (Streamline result {})",
                            static_cast<int>(result));
            }
            state.frame_generation_attached = true;
            LOG_INFO(Render_Vulkan, "NVIDIA DLSS Frame Generation is available");
        }
    }
    return true;
}

bool IsFrameGenerationAvailable() {
    const State& state = GetState();
    return state.started && state.attached && state.frame_generation_attached;
}

void NoteUpscaledFrame(u32 index) {
    g_upscaled_frame.store(index, std::memory_order_release);
}

std::optional<u32> TakeUpscaledFrame() {
    const u32 index = g_upscaled_frame.exchange(~0u, std::memory_order_acq_rel);
    return index == ~0u ? std::nullopt : std::optional<u32>{index};
}

void BeginPresent(std::optional<u32> frame) {
    State& state = GetState();
    if (!IsFrameGenerationAvailable()) {
        return;
    }
    const Api& api = state.api;
    // Frames DLSS did not upscale (menus, loading, videos) have no inputs to generate from.
    const sl::DLSSGMode mode = frame ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
    // The multiplier setting (2x: one generated frame), EDEN_NX2_DLSSG_FRAMES=<generated> for
    // development, within what DLSS-G reports it can generate here.
    u32 frames = static_cast<u32>(Settings::values.dlss_frame_generation_multiplier.GetValue()) + 1;
    if (const char* const value = std::getenv("EDEN_NX2_DLSSG_FRAMES")) {
        frames = static_cast<u32>(std::clamp(std::atoi(value), 1, 3));
    }
    if (state.fg_frames_max == 0) {
        sl::DLSSGState fg_state{};
        if (api.dlssg_get_state(sl::ViewportHandle{0u}, fg_state, nullptr) == sl::Result::eOk &&
            fg_state.numFramesToGenerateMax != 0) {
            state.fg_frames_max = fg_state.numFramesToGenerateMax;
            LOG_INFO(Render_Vulkan, "DLSS Frame Generation can generate up to {} frames",
                     state.fg_frames_max);
        }
    }
    if (state.fg_frames_max != 0) {
        frames = (std::min)(frames, state.fg_frames_max);
    }
    if (!state.fg_mode_set || mode != state.fg_mode || frames != state.fg_frames) {
        state.fg_frames = frames;
        sl::DLSSGOptions options{};
        options.mode = mode;
        options.numFramesToGenerate = frames;
        options.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
        const sl::Result result = api.dlssg_set_options(sl::ViewportHandle{0u}, options);
        LOG_INFO(Render_Vulkan, "DLSS Frame Generation {} ({} generated per frame; result {})",
                 mode == sl::DLSSGMode::eOn ? "on" : "off", options.numFramesToGenerate,
                 static_cast<int>(result));
        state.fg_mode = mode;
        state.fg_mode_set = true;
    }
    if (frame) {
        sl::FrameToken* token{};
        if (api.get_new_frame_token(token, &*frame) == sl::Result::eOk && token) {
            api.pcl_set_marker(sl::PCLMarker::ePresentStart, *token);
        }
    }
    if (++state.fg_presents % 600 == 0 && mode == sl::DLSSGMode::eOn) {
        sl::DLSSGState fg_state{};
        if (api.dlssg_get_state(sl::ViewportHandle{0u}, fg_state, nullptr) == sl::Result::eOk) {
            LOG_INFO(Render_Vulkan,
                     "DLSS Frame Generation: status {:#x}, {} frames presented per real frame, "
                     "minimum size {}",
                     static_cast<u32>(fg_state.status), fg_state.numFramesActuallyPresented,
                     fg_state.minWidthOrHeight);
        }
    }
}

void EndPresent(std::optional<u32> frame) {
    const State& state = GetState();
    if (!frame || !IsFrameGenerationAvailable()) {
        return;
    }
    const Api& api = state.api;
    sl::FrameToken* token{};
    if (api.get_new_frame_token(token, &*frame) == sl::Result::eOk && token) {
        api.pcl_set_marker(sl::PCLMarker::ePresentEnd, *token);
    }
}

bool IsDlssAvailable() {
    const State& state = GetState();
    return state.started && state.attached;
}

void Stop() {
    State& state = GetState();
    if (!state.started) {
        return;
    }
    const sl::Result result = state.api.shutdown();
    state.started = false;
    state.attached = false;
    LOG_INFO(Render_Vulkan, "NVIDIA Streamline stopped (result {})", static_cast<int>(result));
}

const Api& GetApi() {
    return GetState().api;
}

} // namespace Vulkan::Streamline
