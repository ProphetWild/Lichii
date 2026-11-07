// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2018 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <vector>

#include <boost/icl/interval_set.hpp>
#include "common/container/unordered_map.h"
#include "common/container/unordered_set.h"

#include "dynarmic/ir/location_descriptor.h"

namespace Dynarmic::Backend {

template<typename P>
class BlockRangeInformation {
public:
    void AddRange(boost::icl::discrete_interval<P> range, IR::LocationDescriptor location);
    void ClearCache();
    ::Common::unordered_set<IR::LocationDescriptor> InvalidateRanges(const boost::icl::interval_set<P>& invalidated);

private:
    /// The guest code [first, last] a block was translated from. An interval map of block sets
    /// held ~300 bytes per block, so these are bucketed by the guest page of first instead: guests
    /// invalidate one 64-byte line per IC IVAU, so a lookup must not scan every block.
    struct Range {
        P first;
        P last;
        IR::LocationDescriptor location;
    };
    static constexpr unsigned page_bits = 12;
    ::Common::unordered_map<P, std::vector<Range>> pages;  // keyed by first >> page_bits
    /// The most pages any block reaches past the page of its first, so lookups know how far back to look.
    P max_page_span = 0;
};

}  // namespace Dynarmic::Backend
