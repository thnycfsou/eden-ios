// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "common/page_table.h"

TEST_CASE("PageTable: canonical process addresses", "[common]") {
    constexpr u64 address_39 = 0x43ac919c10ULL;
    constexpr u64 sign_extended_39 = 0xffffffc3ac919c10ULL;

    REQUIRE(Common::PageTable::CanonicalizeAddress(address_39, 39) == address_39);
    REQUIRE(Common::PageTable::CanonicalizeAddress(sign_extended_39, 39) == address_39);
    REQUIRE_FALSE(Common::PageTable::CanonicalizeAddress(0xffffff43ac919c10ULL, 39));
    REQUIRE_FALSE(Common::PageTable::CanonicalizeAddress(0, 0));
    REQUIRE_FALSE(Common::PageTable::CanonicalizeAddress(0, 65));
    REQUIRE(Common::PageTable::CanonicalizeAddress(0x123456789abcdef0ULL, 64) ==
            0x123456789abcdef0ULL);

    constexpr u64 address_32 = 0x7ffff000ULL;
    REQUIRE(Common::PageTable::CanonicalizeAddress(address_32, 32) == address_32);
    REQUIRE(Common::PageTable::CanonicalizeAddress(0xffffffff80000000ULL, 32) == 0x80000000ULL);
    REQUIRE_FALSE(Common::PageTable::CanonicalizeAddress(0x0000000180000000ULL, 32));

    constexpr u64 address_36 = 0x8abcdef00ULL;
    REQUIRE(Common::PageTable::CanonicalizeAddress(address_36, 36) == address_36);
    REQUIRE(Common::PageTable::CanonicalizeAddress(0xfffffff08abcdef00ULL, 36) == address_36);
    REQUIRE_FALSE(Common::PageTable::CanonicalizeAddress(0x000000108abcdef00ULL, 36));
}

TEST_CASE("PageTable: canonical process range limits", "[common]") {
    constexpr u64 address_space_end = 1ULL << 39;
    constexpr u64 sign_extended_last_page = 0xffffffc000000000ULL;

    REQUIRE(Common::PageTable::ContainsAddressRange(address_space_end - 4, 4, 39));
    REQUIRE_FALSE(Common::PageTable::ContainsAddressRange(address_space_end - 4, 5, 39));
    REQUIRE(Common::PageTable::ContainsAddressRange(0x3ffc, 8, 39));
    REQUIRE(Common::PageTable::ContainsAddressRange(sign_extended_last_page, 0x1000, 39));
    REQUIRE_FALSE(Common::PageTable::ContainsAddressRange(0xffffff8000000000ULL, 1, 39));
    REQUIRE(Common::PageTable::ContainsAddressRange(~u64{}, 0, 64));
    REQUIRE_FALSE(Common::PageTable::ContainsAddressRange(~u64{}, 1, 64));
    REQUIRE_FALSE(Common::PageTable::ContainsAddressRange(~u64{}, 2, 64));
}

TEST_CASE("PageTable: atomic mapping replacement", "[common]") {
    Common::PageTable::PageEntryData entry;

    entry.Store(false, Common::PageType::Memory, 17, 0x12345000);
    auto [first_pointer, first_type, first_block] = entry.PointerTypeBlock();
    REQUIRE(first_pointer == 0x12345000);
    REQUIRE(first_type == Common::PageType::Memory);
    REQUIRE(first_block == 17);

    entry.Store(false, Common::PageType::DebugMemory, 29, 0x22345000);
    auto [second_pointer, second_type, second_block] = entry.PointerTypeBlock();
    REQUIRE(second_pointer == 0x22345000);
    REQUIRE(second_type == Common::PageType::DebugMemory);
    REQUIRE(second_block == 29);

    entry.Store(false, Common::PageType::Unmapped, 0, 0);
    auto [unmapped_pointer, unmapped_type, unmapped_block] = entry.PointerTypeBlock();
    REQUIRE(unmapped_pointer == 0);
    REQUIRE(unmapped_type == Common::PageType::Unmapped);
    REQUIRE(unmapped_block == 0);
}
