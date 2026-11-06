// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <cstdlib>

#include "common/common_types.h"

namespace Common {

/// HLE service request time, for frame-time diagnosis (EDEN_NX2_HITCH_LOG): the total spent in
/// service handlers and the slowest request since the last Take().
class HleRequestStats {
public:
    struct Slowest {
        u64 ns;
        const char* service;
        const char* function;
        u32 command;
    };

    [[nodiscard]] static bool Enabled() {
        static const bool enabled = std::getenv("EDEN_NX2_HITCH_LOG") != nullptr;
        return enabled;
    }

    void Note(u64 ns, const char* service, const char* function, u32 command) {
        total_ns.fetch_add(ns, std::memory_order_relaxed);
        // Racy by design: a concurrent slower request may be dropped, which is fine for a log.
        if (ns > slowest_ns.load(std::memory_order_relaxed)) {
            slowest_ns.store(ns, std::memory_order_relaxed);
            slowest_service.store(service, std::memory_order_relaxed);
            slowest_function.store(function, std::memory_order_relaxed);
            slowest_command.store(command, std::memory_order_relaxed);
        }
    }

    /// The total time since the last call, and the slowest request (reset).
    [[nodiscard]] std::pair<u64, Slowest> Take() {
        const u64 total = total_ns.exchange(0, std::memory_order_relaxed);
        const Slowest slowest{
            .ns = slowest_ns.exchange(0, std::memory_order_relaxed),
            .service = slowest_service.load(std::memory_order_relaxed),
            .function = slowest_function.load(std::memory_order_relaxed),
            .command = slowest_command.load(std::memory_order_relaxed),
        };
        return {total, slowest};
    }

private:
    std::atomic<u64> total_ns{};
    std::atomic<u64> slowest_ns{};
    std::atomic<const char*> slowest_service{};
    std::atomic<const char*> slowest_function{};
    std::atomic<u32> slowest_command{};
};

inline HleRequestStats g_hle_request_stats;

} // namespace Common
