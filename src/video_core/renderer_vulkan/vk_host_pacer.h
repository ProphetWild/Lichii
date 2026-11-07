// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <chrono>
#include <deque>
#include <optional>
#include <vector>

#include "common/common_types.h"

namespace Vulkan {

/// Host frame pacer (EDEN_NX2_HOST_PACE, live switch; 1 = on, the default, 0 = measure only).
///
/// A swapchain shows a frame at the first display refresh after it is ready. A 60 fps game on a
/// 120 Hz display should then show every frame for two refreshes, but when its frames reach the
/// present thread unevenly (the GPU thread's work varies frame to frame) one lands just after a
/// refresh and stays three refreshes (25 ms), the next just before one and stays one (8 ms); with
/// mailbox two frames ready within one refresh drop the first. The pacer learns when each frame
/// was shown (VK_KHR_present_wait) and the display's refresh period, and holds the next frame
/// until shortly before the refresh `k` refreshes after the previous one, `k` being the game's
/// frame period in refreshes. A frame that is ready later is presented at once.
///
/// With immediate presents (a variable refresh rate display shows each frame when it arrives)
/// there is no grid: frames are held until the game's frame period after the previous one was
/// shown, which only matters when the game holds its period (Ocarina's 30 FPS mode).
///
/// Holds frames on the present thread (asynchronous presentation); without it, and under DLSS
/// Frame Generation, it does not hold them. Reports how long frames were shown every 10 s.
class HostPacer {
public:
    using Clock = std::chrono::steady_clock;

    /// A frame reached the present thread (for the game's frame period).
    void NoteQueued(Clock::time_point queued);

    /// Whether frames are held now (switch, refresh period known).
    [[nodiscard]] bool Pacing() const {
        return pacing;
    }

    /// Re-reads the switch and the refresh period of the display showing `window` (native
    /// handle, may be null) now and then. `variable_refresh`: frames are shown when presented
    /// (immediate present mode on a variable refresh rate display), not on a refresh grid.
    void Refresh(Clock::time_point now, bool allowed, bool variable_refresh, void* window);

    /// When the next frame should be handed to the swapchain; `now` or earlier = at once.
    [[nodiscard]] Clock::time_point Target(Clock::time_point now);

    /// The frame was presented with `id` (0: not presented); `was_held` if Target made it wait.
    void NoteSubmitted(u64 id, Clock::time_point submitted_at, bool was_held);

    /// Target made the present thread wait `hold`.
    void NoteHold(std::chrono::nanoseconds hold);

    /// vkWaitForPresentKHR returned for `id` at `shown`, or the present was lost.
    void NoteShown(u64 id, Clock::time_point shown, bool was_lost);

    /// The last presented id, which the next Target wants shown first (0: none outstanding).
    [[nodiscard]] u64 Outstanding() const {
        return outstanding;
    }

    [[nodiscard]] std::chrono::nanoseconds RefreshPeriod() const {
        return std::chrono::nanoseconds{static_cast<s64>(refresh_ns)};
    }

private:
    struct Submitted {
        u64 id;
        Clock::time_point submitted;
        std::optional<Clock::time_point> intended; // the refresh it was held for
    };

    void Report(Clock::time_point now);

    bool pacing{};
    bool variable{};
    double refresh_ns{1e9 / 60.0};
    bool refresh_known{};
    Clock::time_point next_refresh_query{};

    std::optional<Clock::time_point> last_queued;
    double content_ns{1e9 / 60.0};

    std::optional<Clock::time_point> vblank; // refresh the last shown frame appeared at
    std::optional<Clock::time_point> last_shown;
    u64 last_shown_id{};
    std::optional<Clock::time_point> pending_intended;
    double lead_ns{2.0e6};
    double hold_ema_ns{}; ///< Variable refresh: how long frames are held, on average
    u32 early_streak{};   ///< Frames in a row that could have been shown a refresh earlier
    u64 outstanding{};
    std::deque<Submitted> submitted;

    // Reported every 10 s.
    Clock::time_point report_start{};
    std::array<u32, 6> refreshes_shown{}; // 0 (same refresh), 1, 2, 3, 4, 5+
    u32 frames{}, held{}, missed{}, lost{}, caught_up{}, off_grid{};
    std::vector<double> intervals_ms;
    double interval_sum_ms{}, interval_max_ms{}, hold_sum_ms{}, latency_sum_ms{};
};

/// How evenly frames reach the swapchain, reported every 10 s: what can be measured when present
/// wait is not available (DLSS Frame Generation presents through its own swapchain, and paces
/// the frames it generates between these).
class FrameArrivals {
public:
    void Note(std::chrono::steady_clock::time_point now);

private:
    std::optional<std::chrono::steady_clock::time_point> last;
    std::chrono::steady_clock::time_point report_start{};
    std::vector<double> intervals_ms;
};

/// The refresh period of the display showing `window` (Windows); nullopt elsewhere or on failure.
[[nodiscard]] std::optional<double> QueryRefreshPeriodNs(void* window);

} // namespace Vulkan
