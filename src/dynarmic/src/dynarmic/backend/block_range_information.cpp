// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2018 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include "dynarmic/backend/block_range_information.h"

#include <algorithm>

#include <boost/icl/interval_set.hpp>
#include "common/common_types.h"
#include "common/container/unordered_set.h"

namespace Dynarmic::Backend {

template<typename P>
void BlockRangeInformation<P>::AddRange(boost::icl::discrete_interval<P> range, IR::LocationDescriptor location) {
    if (boost::icl::is_empty(range)) {
        return;
    }
    const P first = boost::icl::first(range);
    const P last = boost::icl::last(range);
    max_page_span = std::max<P>(max_page_span, (last >> page_bits) - (first >> page_bits));
    pages[first >> page_bits].push_back({first, last, location});
}

template<typename P>
void BlockRangeInformation<P>::ClearCache() {
    pages.clear();
    max_page_span = 0;
}

template<typename P>
::Common::unordered_set<IR::LocationDescriptor> BlockRangeInformation<P>::InvalidateRanges(const boost::icl::interval_set<P>& invalidated) {
    ::Common::unordered_set<IR::LocationDescriptor> erase_locations;
    const auto scan = [&](std::vector<Range>& bucket, P lo, P hi) {
        std::erase_if(bucket, [&](const Range& range) {
            if (range.first <= hi && lo <= range.last) {
                erase_locations.insert(range.location);
                return true;
            }
            return false;
        });
    };
    for (const auto& interval : invalidated) {
        const P lo = boost::icl::first(interval);
        const P hi = boost::icl::last(interval);
        // A block overlapping [lo, hi] starts at most max_page_span pages before lo's page.
        const P first_page = (lo >> page_bits) - std::min<P>(lo >> page_bits, max_page_span);
        const P last_page = hi >> page_bits;
        if (static_cast<u64>(last_page - first_page) >= pages.size()) {
            for (auto& [page, bucket] : pages) {
                if (page >= first_page && page <= last_page) {
                    scan(bucket, lo, hi);
                }
            }
        } else {
            // Stops at last_page rather than past it so the top page of the address space cannot wrap.
            for (P page = first_page;; ++page) {
                if (auto it = pages.find(page); it != pages.end()) {
                    scan(it->second, lo, hi);
                }
                if (page == last_page) {
                    break;
                }
            }
        }
    }
    return erase_locations;
}

template class BlockRangeInformation<u32>;
template class BlockRangeInformation<u64>;

}  // namespace Dynarmic::Backend
