// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"

namespace Common::GuestCoreClock {

/// Registers the calling thread as a guest CPU core, for frame-time diagnosis
/// (EDEN_NX2_HITCH_LOG); a no-op otherwise.
void RegisterCurrentThread();

/// The CPU cycles all registered guest core threads have used so far (Windows: thread cycle
/// time, which counts at the TSC rate), or 0 where unsupported.
u64 TotalCycles();

} // namespace Common::GuestCoreClock
