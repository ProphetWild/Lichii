// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdlib>
#include <mutex>
#include <vector>

#include "common/guest_core_clock.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace Common::GuestCoreClock {
namespace {

bool Enabled() {
    static const bool enabled = std::getenv("EDEN_NX2_HITCH_LOG") != nullptr;
    return enabled;
}

std::mutex mutex;
#ifdef _WIN32
std::vector<HANDLE> threads;
#endif

} // Anonymous namespace

void RegisterCurrentThread() {
    if (!Enabled()) {
        return;
    }
#ifdef _WIN32
    HANDLE handle{};
    if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &handle,
                        THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0)) {
        std::scoped_lock lock{mutex};
        threads.push_back(handle);
    }
#endif
}

u64 TotalCycles() {
#ifdef _WIN32
    if (!Enabled()) {
        return 0;
    }
    std::scoped_lock lock{mutex};
    u64 total = 0;
    for (const HANDLE thread : threads) {
        ULONG64 cycles = 0;
        if (QueryThreadCycleTime(thread, &cycles)) {
            total += cycles;
        }
    }
    return total;
#else
    return 0;
#endif
}

} // namespace Common::GuestCoreClock
