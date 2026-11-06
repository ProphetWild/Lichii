// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <cstdint>

namespace Dynarmic {

/// Process-wide JIT work, for frame-time diagnosis: basic blocks translated and emitted, the
/// time it took, and whole-cache invalidations (each forces everything to be compiled again).
struct JitStats {
    std::atomic<std::uint64_t> compiled_blocks{};
    std::atomic<std::uint64_t> compile_ns{};
    std::atomic<std::uint64_t> cache_flushes{};
};

inline JitStats g_jit_stats;

} // namespace Dynarmic
