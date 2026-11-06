// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

#include <fmt/format.h>

#include "common/common_types.h"

/// EDEN_NX2_WAIT_PROFILE=1: how long each guest thread blocks in the guest kernel and on what,
/// per presented frame (Nx2Profile::FrameMark's hitch lines report the largest).
namespace Common::GuestWaitProfile {

enum class Kind : u8 { Object, Mutex, CondVar, Arbiter, Sleep, Run };

inline bool Enabled() {
    static const bool enabled = std::getenv("EDEN_NX2_WAIT_PROFILE") != nullptr;
    return enabled;
}

struct State {
    std::mutex mutex;
    /// (guest thread, kind, object type) -> nanoseconds blocked since the last TakeFrame.
    std::map<std::tuple<u64, Kind, const char*>, u64> waits;
};

inline State& GetState() {
    static State state;
    return state;
}

inline void Add(u64 thread_id, Kind kind, const char* object, u64 ns) {
    State& state = GetState();
    std::scoped_lock lock{state.mutex};
    state.waits[{thread_id, kind, object}] += ns;
}

/// Since the last call (and resets): the `top` threads that ran longest, then the `top` longest
/// waits, as " t<id> <kind>[ <object>] <ms>".
inline std::string TakeFrame(size_t top) {
    State& state = GetState();
    std::vector<std::pair<u64, std::tuple<u64, Kind, const char*>>> sorted;
    {
        std::scoped_lock lock{state.mutex};
        for (const auto& [key, ns] : state.waits) {
            sorted.emplace_back(ns, key);
        }
        state.waits.clear();
    }
    std::ranges::sort(sorted, [](const auto& a, const auto& b) { return a.first > b.first; });
    static constexpr const char* names[] = {"wait", "mutex", "condvar", "arbiter", "sleep", "run"};
    std::string runs;
    std::string waits;
    size_t run_count = 0;
    size_t wait_count = 0;
    for (const auto& [ns, key] : sorted) {
        const auto& [thread, kind, object] = key;
        const bool run = kind == Kind::Run;
        if ((run ? run_count : wait_count) >= top) {
            continue;
        }
        ++(run ? run_count : wait_count);
        (run ? runs : waits) +=
            fmt::format(" t{} {}{}{} {:.1f}", thread, run ? "" : names[static_cast<size_t>(kind)],
                        object ? " " : "", object ? object : "", static_cast<double>(ns) / 1e6);
    }
    return " ran" + runs + "; blocked" + waits;
}

/// Times one blocking wait of the current guest thread.
class Scope {
public:
    Scope(bool active, u64 thread_id_, Kind kind_, const char* object_ = nullptr)
        : thread_id{thread_id_}, kind{kind_}, object{object_},
          start{active && Enabled() ? std::chrono::steady_clock::now()
                                    : std::chrono::steady_clock::time_point{}} {}
    ~Scope() {
        if (start != std::chrono::steady_clock::time_point{}) {
            const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - start);
            Add(thread_id, kind, object, static_cast<u64>(ns.count()));
        }
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    u64 thread_id;
    Kind kind;
    const char* object;
    std::chrono::steady_clock::time_point start;
};

} // namespace Common::GuestWaitProfile
