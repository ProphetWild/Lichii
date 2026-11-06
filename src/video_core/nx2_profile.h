// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#ifdef _MSC_VER
#include <intrin.h>
#elif defined(ARCHITECTURE_x86_64)
#include <x86intrin.h>
#endif
#include <chrono>
#include <cstdlib>
#include <functional>
#include <utility>
#include <string>
#include <unordered_set>

#include "common/guest_wait_profile.h"
#include "common/common_types.h"
#include "video_core/gpu_split.h"
#include "common/logging.h"
#include "common/cpu_features.h"
#include "common/thread_cpu_clock.h"
#include "dynarmic/interface/jit_stats.h"
#include "common/guest_core_clock.h"
#include "common/hle_request_stats.h"

// EDEN_NX2_GPU_PROFILE: time where the GPU thread spends its time. Each zone counts its own
// (exclusive) time, nested zones excluded; every 10 s the thread logs each zone's share.
namespace VideoCommon::Nx2Profile {

enum class Zone : u32 {
    Other, ///< GPU thread time outside every zone below (register writes, command parsing)
    Idle,
    PendingSleep,
    ResumePending,
    Push,
    Tick,
    Run,
    FlushInvalidate,
    Macro,
    Draw,
    Clear,
    Compute,
    FlushWork,
    PipelineLookup,
    Configure,
    SyncDescriptors,
    ReadHandles,
    FillImageViews,
    TextureBuffers,
    UpdateBuffers,
    DescriptorAcquire,
    BindStageBuffers,
    PushDescriptors,
    RenderTargets,
    ConfigureDraw,
    DynamicState,
    DrawRecord,
    TextureUpload,
    Fence,        ///< Fences made (SignalFence)
    FenceSkipped, ///< Empty fences not made
    Method,       ///< Engine method calls (what a split GPU thread's back half would run)
    SplitDrain,   ///< Split GPU thread: the front waits for the back (gpu_split.h)
    SplitWait,    ///< Split GPU thread: the back waits for work
    WriterWait,
    ListSubmitWait,
    ListGpuWait,
    Count,
};

inline constexpr std::array<const char*, static_cast<size_t>(Zone::Count)> zone_names{
    "other",         "idle",          "pending sleep",  "resume pending", "push",
    "tick",          "run",           "flush/inval",    "macro",          "draw",
    "clear",         "compute",       "flush work",     "pipeline lookup", "configure",
    "sync descriptors", "read handles", "fill image views", "texture buffers", "update buffers",
    "descriptor acquire", "bind stage buffers", "push descriptors", "render targets",
    "configure draw", "dynamic state", "draw record", "texture upload", "fence",
    "fence skipped", "method", "split drain", "split wait",
    "writer wait", "list submit wait", "list GPU wait",
};

// Read once at startup: Enabled() runs several times per method write (no guard to check).
inline const bool g_enabled = std::getenv("EDEN_NX2_GPU_PROFILE") != nullptr;

inline bool Enabled() {
    return g_enabled;
}

// Nanoseconds; from the host's own counter where the wall clock reads it directly (the zones
// run ~150,000 times a second, and QueryPerformanceCounter cost ~5% of the GPU thread).
inline u64 Now() {
    return static_cast<u64>(Common::g_wall_clock.GetTimeNS().count());
}

// What the zones and drain waits measure in: the time stamp counter without the fences the wall
// clock reads it with (those fences cost ~10% of the GPU back thread with ~45,000 draws a
// second). Report converts it with the wall clock.
inline u64 Ticks() {
#ifdef ARCHITECTURE_x86_64
    return __rdtsc();
#else
    return Now();
#endif
}

struct ThreadData {
    std::array<u64, static_cast<size_t>(Zone::Count)> exclusive_ns{};
    std::array<u64, static_cast<size_t>(Zone::Count)> count{};
    std::array<u64, 8> counters{}; ///< See Counter
    std::array<u64, static_cast<size_t>(DrainSite::Count)> drain_ns{};
    std::array<u64, static_cast<size_t>(DrainSite::Count)> drain_count{};
    u64 child_ns{};    ///< Time of the zones nested in the current one
    u64 window_start{};
    u64 window_start_ticks{};
    std::optional<u64> cpu_start_ns;
};

inline thread_local ThreadData t_data;

class Scope {
public:
    explicit Scope(Zone zone_) : zone{zone_}, enabled{Enabled()} {
        if (!enabled) {
            return;
        }
        parent_child_ns = t_data.child_ns;
        t_data.child_ns = 0;
        start = Ticks();
    }

    ~Scope() {
        if (!enabled) {
            return;
        }
        const u64 elapsed = Ticks() - start;
        auto& data = t_data;
        const auto index = static_cast<size_t>(zone);
        data.exclusive_ns[index] += elapsed - data.child_ns;
        ++data.count[index];
        data.child_ns = parent_child_ns + elapsed;
    }

    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    Zone zone;
    bool enabled;
    u64 start{};
    u64 parent_child_ns{};
};

/// Amounts counted on the GPU thread, logged per second with the zones.
enum class Counter : u32 {
    DownloadBytes,  ///< Buffer data copied back from the host GPU at fences
    DownloadCopies, ///< Buffer ranges copied back
    DmaReadBytes,   ///< Guest memory the native MME read with DMA
    DownloadBytesOnDmaPages, ///< Of DownloadBytes: on pages the MME has DMA-read
    MethodWrites,   ///< 3D register writes (Maxwell3D::CallMethod below the macro range)
    SameValueWrites, ///< Of MethodWrites: the register already held the value
    AcquireChecks,  ///< Semaphore acquires evaluated
    AcquireWaits,   ///< Of AcquireChecks: not met yet (the channel waits)
};

/// Method calls on this thread by method (3D registers on the back, macros on the front), and
/// same-value writes among them; MME DMA reads by the macro that made them. The most frequent
/// are logged with the zones.
inline thread_local std::array<u32, 0x2000> t_method_count{};
inline thread_local std::array<u32, 0x2000> t_method_same{};
inline thread_local std::array<u32, 0x2000> t_dma_reads{};

inline void CountMethod(u32 method, bool same) {
    if (Enabled() && method < t_method_count.size()) {
        ++t_method_count[method];
        t_method_same[method] += same ? 1 : 0;
    }
}

inline void CountDmaRead(u32 macro) {
    if (Enabled() && macro < t_dma_reads.size()) {
        ++t_dma_reads[macro];
    }
}

inline std::string TopMethods(const std::array<u32, 0x2000>& counts,
                              const std::array<u32, 0x2000>* same, u64 window) {
    std::array<u32, 0x2000> order;
    for (u32 i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    constexpr size_t top = 24;
    std::partial_sort(order.begin(), order.begin() + top, order.end(),
                      [&counts](u32 a, u32 b) { return counts[a] > counts[b]; });
    std::string text;
    for (size_t i = 0; i < top && counts[order[i]] != 0; ++i) {
        const u32 method = order[i];
        text += fmt::format(" {:#x} {:.0f}/s", method,
                            static_cast<double>(counts[method]) * 1e9 / window);
        if (same != nullptr && (*same)[method] != 0) {
            text += fmt::format(" ({:.0f} same)", static_cast<double>((*same)[method]) * 1e9 / window);
        }
        text += ',';
    }
    if (!text.empty()) {
        text.pop_back();
    }
    return text;
}

/// Device pages (4 KiB) the native MME has DMA-read since start (GPU thread only; filled only
/// while profiling): which buffer downloads anything on the GPU thread reads back.
inline std::unordered_set<u64>& DmaReadPages() {
    static std::unordered_set<u64> pages;
    return pages;
}

inline void Add(Counter counter, u64 amount) {
    if (Enabled()) {
        t_data.counters[static_cast<size_t>(counter)] += amount;
    }
}

/// The front waited for the back since `start` (Ticks) for `site`.
inline void AddDrain(DrainSite site, u64 start) {
    if (Enabled()) {
        const auto index = static_cast<size_t>(site);
        t_data.drain_ns[index] += Ticks() - start;
        ++t_data.drain_count[index];
    }
}

/// Called by the GPU thread (and the split's back thread) between commands: logs and resets
/// the totals every 10 s.
inline void Report(const char* thread_name = "GPU thread") {
    if (!Enabled()) {
        return;
    }
    auto& data = t_data;
    const u64 now = Now();
    if (data.window_start == 0) {
        data.window_start = now;
        data.window_start_ticks = Ticks();
        data.cpu_start_ns = Common::CurrentThreadCpuTimeNs();
        data.child_ns = 0;
        return;
    }
    const u64 window = now - data.window_start;
    if (window < 10'000'000'000ULL) {
        return;
    }
    const auto cpu_now = Common::CurrentThreadCpuTimeNs();
    const u64 now_ticks = Ticks();
    const u64 window_ticks = (std::max<u64>)(now_ticks - data.window_start_ticks, 1);
    // Time in no zone at all is the wall time minus the zones' totals.
    u64 zoned{};
    for (size_t i = 1; i < data.exclusive_ns.size(); ++i) {
        zoned += data.exclusive_ns[i];
    }
    data.exclusive_ns[0] = window_ticks > zoned ? window_ticks - zoned : 0;
    std::string line;
    for (size_t i = 0; i < data.exclusive_ns.size(); ++i) {
        if (data.exclusive_ns[i] == 0 && data.count[i] == 0) {
            continue;
        }
        // Per second of wall time: milliseconds spent and calls made.
        const double ms = static_cast<double>(data.exclusive_ns[i]) * 1000.0 / window_ticks;
        const double calls = static_cast<double>(data.count[i]) * 1e9 / window;
        line += fmt::format(" | {} {:.1f} ms ({:.0f}/s)", zone_names[i], ms, calls);
    }
    line += fmt::format(" | buffer downloads {:.0f} KiB in {:.0f} copies ({:.0f} KiB on pages "
                        "MME DMA reads) | MME DMA reads {:.0f} KiB",
                        static_cast<double>(data.counters[0]) * 1e9 / 1024.0 / window,
                        static_cast<double>(data.counters[1]) * 1e9 / window,
                        static_cast<double>(data.counters[3]) * 1e9 / 1024.0 / window,
                        static_cast<double>(data.counters[2]) * 1e9 / 1024.0 / window);
    line += fmt::format(" | 3D register writes {:.0f} ({:.0f} same value)",
                        static_cast<double>(data.counters[4]) * 1e9 / window,
                        static_cast<double>(data.counters[5]) * 1e9 / window);
    line += fmt::format(" | acquires {:.0f} ({:.0f} waited)",
                        static_cast<double>(data.counters[6]) * 1e9 / window,
                        static_cast<double>(data.counters[7]) * 1e9 / window);
    std::string drains;
    for (size_t i = 0; i < data.drain_count.size(); ++i) {
        if (data.drain_count[i] != 0) {
            drains += fmt::format(" {} {:.1f} ms ({:.0f}/s),", drain_site_names[i],
                                  static_cast<double>(data.drain_ns[i]) * 1000.0 / window_ticks,
                                  static_cast<double>(data.drain_count[i]) * 1e9 / window);
        }
    }
    if (!drains.empty()) {
        drains.pop_back();
        line += " | waits for the back by site:" + drains;
    }
    if (cpu_now && data.cpu_start_ns && *cpu_now >= *data.cpu_start_ns) {
        line += fmt::format(" | host_cpu_busy_ms_per_s={:.3f} window_wall_ms={:.3f}",
                            static_cast<double>(*cpu_now - *data.cpu_start_ns) * 1000.0 / window,
                            static_cast<double>(window) / 1e6);
    } else {
        line += fmt::format(" | host_cpu_busy_ms_per_s=unavailable window_wall_ms={:.3f}",
                            static_cast<double>(window) / 1e6);
    }
    LOG_INFO(HW_GPU, "{} per second:{}", thread_name, line);
    if (const std::string methods = TopMethods(t_method_count, &t_method_same, window); !methods.empty()) {
        LOG_INFO(HW_GPU, "{} methods per second:{}", thread_name, methods);
    }
    if (const std::string reads = TopMethods(t_dma_reads, nullptr, window); !reads.empty()) {
        LOG_INFO(HW_GPU, "{} MME DMA reads per second by macro:{}", thread_name, reads);
    }
    t_method_count = {};
    t_method_same = {};
    t_dma_reads = {};
    data = {};
    data.window_start = now;
    data.window_start_ticks = now_ticks;
    data.cpu_start_ns = cpu_now;
}

/// The GPU (front) thread's zone totals, published for FrameMark on the presenting thread.
inline std::array<std::atomic<u64>, static_cast<size_t>(Zone::Count)> g_front_zones{};

/// Called by the GPU thread at each loop turn while EDEN_NX2_HITCH_LOG is on.
inline void PublishFrontZones() {
    static const bool enabled = std::getenv("EDEN_NX2_HITCH_LOG") != nullptr;
    if (!enabled || !Enabled()) {
        return;
    }
    const auto& zones = t_data.exclusive_ns;
    for (size_t i = 0; i < zones.size(); ++i) {
        g_front_zones[i].store(zones[i], std::memory_order_relaxed);
    }
}

/// What the present path spent on one frame (RendererVulkan::Composite), for FrameMark.
struct PresentTimes {
    double acquire_ms;
    double record_submit_ms;
    double present_ms;
};

/// EDEN_NX2_HITCH_LOG=1 (with EDEN_NX2_GPU_PROFILE=1): called once per presented frame on the
/// presenting thread. A frame much longer than the recent median logs where this thread's time
/// went during it (its zones) and what the present path took, and a summary every 600 frames.
inline void FrameMark(const PresentTimes& present) {
    static const bool enabled = std::getenv("EDEN_NX2_HITCH_LOG") != nullptr;
    if (!enabled || !Enabled()) {
        return;
    }
    struct State {
        std::array<u64, static_cast<size_t>(Zone::Count)> zones{};
        std::array<u64, static_cast<size_t>(Zone::Count)> front_zones{};
        std::array<double, 120> intervals{};
        std::uint64_t jit_blocks{};
        std::uint64_t jit_ns{};
        std::uint64_t jit_flushes{};
        std::uint64_t guest_cycles{};
        std::array<double, 600> guest_busy{};
        u64 frame{};
        u64 last_ns{};
        u64 last_ticks{};
        u32 hitches{};
        double worst_ms{};
    };
    static thread_local State state;
    const u64 now_ns = Now();
    const u64 now_ticks = Ticks();
    // What the guest threads blocked on during this frame (EDEN_NX2_WAIT_PROFILE).
    const std::string guest_waits = Common::GuestWaitProfile::Enabled()
                                        ? " | guest threads (ms):" +
                                              Common::GuestWaitProfile::TakeFrame(8)
                                        : std::string{};
    auto& data = t_data;
    const auto previous_zones = state.zones;
    state.zones = data.exclusive_ns;
    const auto previous_front = state.front_zones;
    for (size_t i = 0; i < state.front_zones.size(); ++i) {
        state.front_zones[i] = g_front_zones[i].load(std::memory_order_relaxed);
    }
    // Guest CPU JIT work since the last frame (all cores).
    const auto& jit = Dynarmic::g_jit_stats;
    const u64 jit_blocks = jit.compiled_blocks.load(std::memory_order_relaxed);
    const u64 jit_ns = jit.compile_ns.load(std::memory_order_relaxed);
    const u64 jit_flushes = jit.cache_flushes.load(std::memory_order_relaxed);
    const u64 frame_jit_blocks = jit_blocks - std::exchange(state.jit_blocks, jit_blocks);
    const double frame_jit_ms =
        static_cast<double>(jit_ns - std::exchange(state.jit_ns, jit_ns)) / 1e6;
    const u64 frame_jit_flushes = jit_flushes - std::exchange(state.jit_flushes, jit_flushes);
    const auto [hle_ns, hle_slowest] = Common::g_hle_request_stats.Take();
    const u64 guest_cycles = Common::GuestCoreClock::TotalCycles();
    const u64 frame_guest_cycles = guest_cycles - std::exchange(state.guest_cycles, guest_cycles);
    if (state.last_ns == 0) {
        state.last_ns = now_ns;
        state.last_ticks = now_ticks;
        return;
    }
    const double interval_ms = static_cast<double>(now_ns - state.last_ns) / 1e6;
    const double ns_per_tick = static_cast<double>(now_ns - state.last_ns) /
                               static_cast<double>((std::max<u64>)(now_ticks - state.last_ticks, 1));
    state.last_ns = now_ns;
    state.last_ticks = now_ticks;
    auto sorted = state.intervals;
    const size_t filled = (std::min<u64>)(state.frame, sorted.size());
    std::sort(sorted.begin(), sorted.begin() + filled);
    const double median = filled != 0 ? sorted[filled / 2] : interval_ms;
    state.intervals[state.frame % state.intervals.size()] = interval_ms;
    const double guest_busy_ms = static_cast<double>(frame_guest_cycles) * ns_per_tick / 1e6;
    state.guest_busy[state.frame % state.guest_busy.size()] = guest_busy_ms;
    ++state.frame;
    if (filled == sorted.size() && interval_ms > (std::max)(median * 1.6, median + 6.0)) {
        ++state.hitches;
        state.worst_ms = (std::max)(state.worst_ms, interval_ms);
        std::array<std::pair<double, size_t>, static_cast<size_t>(Zone::Count)> spent{};
        for (size_t i = 0; i < spent.size(); ++i) {
            const u64 delta = state.zones[i] - previous_zones[i];
            spent[i] = {static_cast<double>(delta) * ns_per_tick / 1e6, i};
        }
        std::sort(spent.begin(), spent.end(), std::greater<>{});
        std::string zones;
        for (size_t i = 0; i < 6 && spent[i].first >= 0.5; ++i) {
            zones += fmt::format(" {} {:.1f}", zone_names[spent[i].second], spent[i].first);
        }
        // The GPU thread's zones (with a split, the front): waits on the back are "split
        // drain", waits for the guest's next command lists "idle"/"resume pending".
        std::array<std::pair<double, size_t>, static_cast<size_t>(Zone::Count)> front{};
        for (size_t i = 0; i < front.size(); ++i) {
            const u64 delta = state.front_zones[i] - previous_front[i];
            front[i] = {static_cast<double>(delta) * ns_per_tick / 1e6, i};
        }
        std::sort(front.begin(), front.end(), std::greater<>{});
        std::string front_zones;
        for (size_t i = 0; i < 5 && front[i].first >= 0.5; ++i) {
            front_zones += fmt::format(" {} {:.1f}", zone_names[front[i].second], front[i].first);
        }
        LOG_INFO(HW_GPU,
                 "Hitch: frame {} took {:.1f} ms (median {:.1f}) | {} zones (ms):{} | GPU thread "
                 "zones (ms):{} | acquire "
                 "{:.1f} record/submit {:.1f} present {:.1f} | guest JIT {} blocks {:.1f} ms{} | "
                 "services {:.1f} ms, slowest {}::{} ({}) {:.1f} ms | guest cores busy {:.1f} ms"
                 "{}",
                 state.frame, interval_ms, median,
                 GpuSplit::OnBackThread() ? "back thread" : "GPU thread",
                 zones.empty() ? " none over 0.5" : zones,
                 front_zones.empty() ? " none over 0.5" : front_zones, present.acquire_ms,
                 present.record_submit_ms, present.present_ms, frame_jit_blocks, frame_jit_ms,
                 frame_jit_flushes != 0 ? fmt::format(", {} cache flushes", frame_jit_flushes)
                                        : std::string{},
                 static_cast<double>(hle_ns) / 1e6,
                 hle_slowest.service ? hle_slowest.service : "-", hle_slowest.command,
                 hle_slowest.function ? hle_slowest.function : "-",
                 static_cast<double>(hle_slowest.ns) / 1e6,
                 static_cast<double>(frame_guest_cycles) * ns_per_tick / 1e6, guest_waits);
    } else if (!guest_waits.empty() && state.frame % 300 == 0) {
        LOG_INFO(HW_GPU, "Frame {} took {:.1f} ms (a normal frame){}", state.frame, interval_ms,
                 guest_waits);
    }
    if (state.frame % 600 == 0) {
        auto busy = state.guest_busy;
        std::nth_element(busy.begin(), busy.begin() + busy.size() / 2, busy.end());
        LOG_INFO(HW_GPU,
                 "Hitches in the last 600 frames: {} (worst {:.1f} ms, median {:.1f} ms); guest "
                 "cores busy per frame: median {:.1f} ms",
                 state.hitches, state.worst_ms, median, busy[busy.size() / 2]);
        state.hitches = 0;
        state.worst_ms = 0;
    }
}

} // namespace VideoCommon::Nx2Profile
