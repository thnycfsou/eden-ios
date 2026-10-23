// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include <boost/icl/interval_set.hpp>

#include "dynarmic/backend/block_range_information.h"

using Dynarmic::Backend::BlockRangeInformation;
using Dynarmic::IR::LocationDescriptor;

TEST_CASE("block range invalidation removes complete affected blocks and preserves others") {
    BlockRangeInformation<u64> ranges;
    const LocationDescriptor block_a{1};
    const LocationDescriptor block_b{2};
    const LocationDescriptor block_c{3};
    ranges.AddRange(boost::icl::discrete_interval<u64>::closed(0, 10), block_a);
    ranges.AddRange(boost::icl::discrete_interval<u64>::closed(5, 15), block_b);
    ranges.AddRange(boost::icl::discrete_interval<u64>::closed(100, 110), block_c);

    boost::icl::interval_set<u64> invalidated;
    invalidated.add(boost::icl::discrete_interval<u64>::closed(1, 2));
    const auto removed = ranges.InvalidateRanges(invalidated);

    REQUIRE(removed.contains(block_a));
    REQUIRE_FALSE(removed.contains(block_b));
    REQUIRE_FALSE(removed.contains(block_c));
    bool retained_block_b = false;
    bool retained_block_c = false;
    for (const auto& [range, descriptors] : ranges.block_ranges) {
        (void)range;
        retained_block_b |= descriptors.contains(block_b);
        retained_block_c |= descriptors.contains(block_c);
    }
    REQUIRE(retained_block_b);
    REQUIRE(retained_block_c);

    boost::icl::interval_set<u64> overlap_invalidation;
    overlap_invalidation.add(boost::icl::discrete_interval<u64>::closed(7, 8));
    const auto removed_overlap = ranges.InvalidateRanges(overlap_invalidation);
    REQUIRE(removed_overlap.contains(block_b));
    REQUIRE_FALSE(removed_overlap.contains(block_c));

    auto remaining_range = ranges.block_ranges.begin();
    REQUIRE(remaining_range != ranges.block_ranges.end());
    REQUIRE(remaining_range->second.contains(block_c));
    ++remaining_range;
    REQUIRE(remaining_range == ranges.block_ranges.end());

    ranges.ClearCache();
    REQUIRE(ranges.block_ranges.empty());

    ranges.AddRange(boost::icl::discrete_interval<u64>::closed(200, 210), block_a);
    boost::icl::interval_set<u64> new_invalidation;
    new_invalidation.add(boost::icl::discrete_interval<u64>::closed(205, 206));
    REQUIRE(ranges.InvalidateRanges(new_invalidation).contains(block_a));
}

TEST_CASE("32-bit block range metadata can be invalidated and cleared") {
    BlockRangeInformation<u32> ranges;
    const LocationDescriptor block_a{1};
    const LocationDescriptor block_b{2};
    ranges.AddRange(boost::icl::discrete_interval<u32>::closed(0, 10), block_a);
    ranges.AddRange(boost::icl::discrete_interval<u32>::closed(5, 15), block_b);

    boost::icl::interval_set<u32> invalidated;
    invalidated.add(boost::icl::discrete_interval<u32>::closed(1, 2));
    const auto removed = ranges.InvalidateRanges(invalidated);

    REQUIRE(removed.contains(block_a));
    REQUIRE_FALSE(removed.contains(block_b));
    REQUIRE(ranges.block_ranges.begin()->second.contains(block_b));

    ranges.ClearCache();
    REQUIRE(ranges.block_ranges.empty());
}
