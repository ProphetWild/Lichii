// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <vector>

#include "common/logging.h"
#include "video_core/nx2_debug_control.h"
#include "video_core/nx2_perf.h"
#include "video_core/renderer_vulkan/vk_host_pacer.h"

#ifdef _WIN32
#include <dwmapi.h>
#include <windows.h>
#pragma comment(lib, "dwmapi.lib")
#endif

namespace Vulkan {
namespace {
using Ns = std::chrono::nanoseconds;
using Ms = std::chrono::duration<double, std::milli>;

Ns ToNs(double ns) {
    return Ns{static_cast<s64>(ns)};
}
} // Anonymous namespace

#ifdef _WIN32
namespace {
/// The refresh rate of the display showing `window` from the display configuration (exact, e.g.
/// 119.88 Hz), or the GDI mode's whole hertz when that fails.
std::optional<double> MonitorRefreshPeriodNs(HWND window) {
    const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (monitor == nullptr || !GetMonitorInfoW(monitor, &info)) {
        return std::nullopt;
    }
    UINT32 path_count = 0;
    UINT32 mode_count = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) ==
        ERROR_SUCCESS) {
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
        if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count,
                               modes.data(), nullptr) == ERROR_SUCCESS) {
            for (UINT32 i = 0; i < path_count; ++i) {
                DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
                source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
                source.header.size = sizeof(source);
                source.header.adapterId = paths[i].sourceInfo.adapterId;
                source.header.id = paths[i].sourceInfo.id;
                const DISPLAYCONFIG_RATIONAL rate = paths[i].targetInfo.refreshRate;
                if (DisplayConfigGetDeviceInfo(&source.header) == ERROR_SUCCESS &&
                    std::wcscmp(source.viewGdiDeviceName, info.szDevice) == 0 &&
                    rate.Numerator != 0 && rate.Denominator != 0) {
                    return 1e9 * rate.Denominator / rate.Numerator;
                }
            }
        }
    }
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) &&
        mode.dmDisplayFrequency > 1) {
        return 1e9 / mode.dmDisplayFrequency;
    }
    return std::nullopt;
}
} // Anonymous namespace
#endif

std::optional<double> QueryRefreshPeriodNs(void* window) {
#ifdef _WIN32
    // The display the game is on: DWM's timing is the primary display's, which may run at another
    // rate (144 Hz primary, game on a 120 Hz TV).
    const std::optional<double> monitor =
        window != nullptr ? MonitorRefreshPeriodNs(static_cast<HWND>(window)) : std::nullopt;
    DWM_TIMING_INFO info{};
    info.cbSize = sizeof(info);
    if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &info)) &&
        info.rateRefresh.uiNumerator != 0 && info.rateRefresh.uiDenominator != 0) {
        const double nominal = 1e9 * info.rateRefresh.uiDenominator / info.rateRefresh.uiNumerator;
        const bool same_display = !monitor || std::abs(*monitor - nominal) < nominal * 0.01;
        // The compositor's measured period: a "120 Hz" display may run at 119.98, and the game's
        // frame clock follows this (genlock). The nominal rate when it looks off.
        LARGE_INTEGER frequency{};
        if (same_display && info.qpcRefreshPeriod != 0 && QueryPerformanceFrequency(&frequency) &&
            frequency.QuadPart != 0) {
            const double measured = 1e9 * static_cast<double>(info.qpcRefreshPeriod) /
                                    static_cast<double>(frequency.QuadPart);
            if (std::abs(measured - nominal) < nominal * 0.01) {
                return measured;
            }
        }
        if (same_display) {
            return nominal;
        }
    }
    if (monitor) {
        return monitor;
    }
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode) &&
        mode.dmDisplayFrequency > 1) {
        return 1e9 / mode.dmDisplayFrequency;
    }
#endif
    return std::nullopt;
}

void FrameArrivals::Note(std::chrono::steady_clock::time_point now) {
    if (last) {
        intervals_ms.push_back(Ms{now - *last}.count());
    } else {
        report_start = now;
    }
    last = now;
    if (now - report_start < std::chrono::seconds{10} || intervals_ms.empty()) {
        return;
    }
    double sum = 0.0;
    for (const double interval : intervals_ms) {
        sum += interval;
    }
    const double mean = sum / static_cast<double>(intervals_ms.size());
    double variance = 0.0;
    for (const double interval : intervals_ms) {
        variance += (interval - mean) * (interval - mean);
    }
    std::sort(intervals_ms.begin(), intervals_ms.end());
    const size_t n = intervals_ms.size();
    LOG_INFO(Render_Vulkan,
             "Frame arrivals: {} frames; interval mean {:.2f} ms, stddev {:.2f}, p1 {:.2f}, p50 "
             "{:.2f}, p99 {:.2f}, max {:.2f}",
             n, mean, std::sqrt(variance / static_cast<double>(n)), intervals_ms[n / 100],
             intervals_ms[n / 2], intervals_ms[n * 99 / 100], intervals_ms.back());
    intervals_ms.clear();
    report_start = now;
}

void HostPacer::NoteQueued(Clock::time_point queued) {
    if (last_queued) {
        // A load screen's or a stall's gap would take seconds to forget: clamp each sample.
        const double sample = std::clamp(static_cast<double>(Ns{queued - *last_queued}.count()),
                                         refresh_ns * 0.5, 100e6);
        content_ns += (sample - content_ns) * 0.05;
    }
    last_queued = queued;
}

void HostPacer::Refresh(Clock::time_point now, bool allowed, bool variable_refresh, void* window) {
    variable = variable_refresh;
    if (now >= next_refresh_query) {
        next_refresh_query = now + std::chrono::seconds{2};
        if (const auto period = QueryRefreshPeriodNs(window);
            period && *period > 2e6 && *period < 50e6) {
            if (!refresh_known || std::abs(*period - refresh_ns) > refresh_ns * 0.01) {
                LOG_INFO(Render_Vulkan, "Host pacing: display refresh {:.4f} Hz", 1e9 / *period);
                vblank.reset();
            }
            refresh_ns = *period;
            refresh_known = true;
        }
    }
    pacing = allowed && refresh_known &&
             VideoCore::Nx2Debug::SwitchValue(VideoCore::Nx2Debug::g_host_pace,
                                              "EDEN_NX2_HOST_PACE", 1) != 0;
}

HostPacer::Clock::time_point HostPacer::Target(Clock::time_point now) {
    pending_intended.reset();
    if (!pacing || !vblank || outstanding != 0) {
        return now;
    }
    // Refreshes per game frame, from the frame rate the game asks for: 2 for 60 fps at 120 Hz,
    // also while it runs at 55 (those frames then show for 2 or 3), 4 for 30 fps. Not from how
    // fast frames arrive: holding frames slows the game, and the pacer then locked itself to
    // the slower rate (a loading screen's 25 ms frames held Ocarina at 40 fps). Unknown: only
    // one frame per refresh.
    const long long guest_ns =
        VideoCommon::Nx2Perf::g_guest_frame_interval_ns.load(std::memory_order_relaxed);
    if (variable) {
        // A variable refresh rate display shows each frame when it is presented: hold it until
        // the game's frame period after the previous one was shown, so frames finished early
        // (the GPU thread's work varies frame to frame) are shown evenly. Late ones go at once.
        if (guest_ns <= 0 || !last_shown) {
            return now;
        }
        // A late frame moves this schedule later for good, and the frames after it then wait
        // longer (input lag): while they are held more than ~3 ms on average, show each up to
        // 0.5 ms early until the wait is back.
        const double shorten = std::clamp((hold_ema_ns - 3e6) * 0.1, 0.0, 0.5e6);
        const auto intended = *last_shown + Ns{guest_ns} - ToNs(shorten);
        const auto target = intended - ToNs(lead_ns);
        hold_ema_ns +=
            ((std::max)(0.0, static_cast<double>(Ns{target - now}.count())) - hold_ema_ns) * 0.1;
        if (target <= now) {
            return now;
        }
        pending_intended = intended;
        return target;
    }
    const s64 k =
        guest_ns > 0
            ? (std::max<s64>)(1, static_cast<s64>(std::floor(guest_ns / refresh_ns + 0.02)))
            : 1;
    auto intended = *vblank + ToNs(static_cast<double>(k) * refresh_ns);
    // A late frame moves the schedule a refresh later for good, and the frames after it then
    // wait that refresh in the queue (input lag). Once frames could have been shown a refresh
    // earlier for 2 s (1 s at 30 fps and below) in a row, show one a refresh early (one short
    // frame) to drop that wait. At 30 fps without it, Ocarina's frames waited ~30 ms each after
    // a few hitches: nearly a frame of extra input lag.
    if (k >= 2 && intended - ToNs(refresh_ns + lead_ns + 1e6) > now) {
        if (++early_streak >= (k == 2 ? 120U : 30U)) {
            early_streak = 0;
            intended -= ToNs(refresh_ns);
            ++caught_up;
        }
    } else {
        early_streak = 0;
    }
    const auto target = intended - ToNs(lead_ns);
    if (target <= now) {
        return now;
    }
    pending_intended = intended;
    return target;
}

void HostPacer::NoteSubmitted(u64 id, Clock::time_point submitted_at, bool was_held) {
    if (id == 0) {
        outstanding = 0;
        pending_intended.reset();
        return;
    }
    submitted.push_back({id, submitted_at, was_held ? pending_intended : std::nullopt});
    while (submitted.size() > 16) {
        submitted.pop_front();
    }
    pending_intended.reset();
    outstanding = id;
}

void HostPacer::NoteShown(u64 id, Clock::time_point shown, bool was_lost) {
    while (!submitted.empty() && submitted.front().id < id) {
        submitted.pop_front();
    }
    std::optional<Submitted> entry;
    if (!submitted.empty() && submitted.front().id == id) {
        entry = submitted.front();
        submitted.pop_front();
    }
    if (outstanding != 0 && id >= outstanding) {
        outstanding = 0;
    }
    if (report_start == Clock::time_point{}) {
        report_start = shown;
    }
    if (was_lost) {
        ++lost;
        vblank.reset();
        last_shown.reset();
        return;
    }
    ++frames;
    if (entry) {
        latency_sum_ms += Ms{shown - entry->submitted}.count();
    }
    if (last_shown && last_shown_id + 1 == id) {
        const double interval = static_cast<double>(Ns{shown - *last_shown}.count());
        interval_sum_ms += interval / 1e6;
        interval_max_ms = (std::max)(interval_max_ms, interval / 1e6);
        intervals_ms.push_back(interval / 1e6);
        const auto count = std::clamp<s64>(std::llround(interval / refresh_ns), 0, 5);
        ++refreshes_shown[static_cast<size_t>(count)];
        // Far from a whole number of refreshes: a variable refresh rate display (or bad timing).
        if (std::abs(interval - static_cast<double>(std::llround(interval / refresh_ns)) *
                                    refresh_ns) > refresh_ns * 0.2) {
            ++off_grid;
        }
    }
    // Follow the refresh grid: snap to the refresh this was shown at and correct the phase a
    // little, so the wake-up noise of the wait does not move the targets.
    if (vblank) {
        const double since = static_cast<double>(Ns{shown - *vblank}.count());
        const s64 refreshes = std::llround(since / refresh_ns);
        if (refreshes >= 1) {
            const auto predicted = *vblank + ToNs(static_cast<double>(refreshes) * refresh_ns);
            const double residual = static_cast<double>(Ns{shown - predicted}.count());
            vblank =
                std::abs(residual) < refresh_ns * 0.5 ? predicted + ToNs(residual * 0.1) : shown;
        }
    } else {
        vblank = shown;
    }
    if (entry && entry->intended && variable) {
        // The lead is the present-to-shown time: keep the shown time within ~0.5 ms after the
        // intended one.
        const double error = static_cast<double>(Ns{shown - *entry->intended}.count());
        if (error > 0.5e6) {
            ++missed;
            lead_ns = (std::min)(lead_ns + (std::min)(error, 2e6) * 0.25, 8e6);
        } else if (error < -0.25e6) {
            lead_ns = (std::max)(lead_ns + error * 0.1, 0.2e6);
        }
    } else if (entry && entry->intended) {
        if (shown > *entry->intended + ToNs(refresh_ns * 0.5)) {
            ++missed;
            lead_ns = (std::min)(lead_ns + 0.5e6, refresh_ns * 0.75);
        } else {
            lead_ns = (std::max)(lead_ns - 0.01e6, 0.5e6);
        }
    }
    last_shown = shown;
    last_shown_id = id;
    Report(shown);
}

void HostPacer::NoteHold(Ns hold) {
    ++held;
    hold_sum_ms += Ms{hold}.count();
}

void HostPacer::Report(Clock::time_point now) {
    if (now - report_start < std::chrono::seconds{10} || frames == 0) {
        return;
    }
    u32 intervals = 0;
    for (const u32 count : refreshes_shown) {
        intervals += count;
    }
    // Frame time consistency of what was shown: the spread of the intervals and the slowest 1%.
    double stddev_ms = 0.0;
    double p99_ms = 0.0;
    double p1_ms = 0.0;
    double p50_ms = 0.0;
    if (!intervals_ms.empty()) {
        const double mean = interval_sum_ms / static_cast<double>(intervals_ms.size());
        for (const double interval : intervals_ms) {
            stddev_ms += (interval - mean) * (interval - mean);
        }
        stddev_ms = std::sqrt(stddev_ms / static_cast<double>(intervals_ms.size()));
        std::sort(intervals_ms.begin(), intervals_ms.end());
        p1_ms = intervals_ms[intervals_ms.size() / 100];
        p50_ms = intervals_ms[intervals_ms.size() / 2];
        p99_ms = intervals_ms[intervals_ms.size() * 99 / 100];
    }
    LOG_INFO(
        Render_Vulkan,
        "Host pacing {}{}: {} frames at {:.3f} Hz; shown for 1/2/3/4/5+ refreshes: "
        "{}/{}/{}/{}/{} (same refresh {}, off grid {}); interval mean {:.2f} ms, stddev {:.2f}, "
        "p1 {:.2f}, p50 {:.2f}, p99 {:.2f}, max {:.2f}; held {} "
        "(mean {:.2f} ms, missed {}), lead {:.2f} ms; present to shown {:.2f} ms; game "
        "frame {:.2f} ms; {} lost; caught up {}",
        pacing ? "on" : "off", variable ? " (variable refresh)" : "", frames, 1e9 / refresh_ns,
        refreshes_shown[1], refreshes_shown[2], refreshes_shown[3], refreshes_shown[4],
        refreshes_shown[5], refreshes_shown[0], off_grid,
        intervals ? interval_sum_ms / intervals : 0.0, stddev_ms, p1_ms, p50_ms, p99_ms,
        interval_max_ms, held, held ? hold_sum_ms / held : 0.0, missed, lead_ns / 1e6,
        latency_sum_ms / frames, content_ns / 1e6, lost, caught_up);
    report_start = now;
    refreshes_shown = {};
    frames = held = missed = lost = caught_up = off_grid = 0;
    intervals_ms.clear();
    interval_sum_ms = interval_max_ms = hold_sum_ms = latency_sum_ms = 0;
}

} // namespace Vulkan
