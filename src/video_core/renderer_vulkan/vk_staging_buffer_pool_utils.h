// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <limits>

namespace Vulkan::StagingBufferPoolUtils {

// VkBuffer sizes need not be powers of two. Keep allocations aligned to the maximum
// alignment used by this pool without reserving the unused tail of the next power of two.
constexpr std::size_t AlignAllocationSize(std::size_t size, std::size_t alignment) noexcept {
    if (size == 0 || alignment == 0 || (alignment & (alignment - 1)) != 0 ||
        size > std::numeric_limits<std::size_t>::max() - (alignment - 1)) {
        return 0;
    }
    return (size + alignment - 1) & ~(alignment - 1);
}

constexpr bool CanReuseAllocation(std::size_t capacity, std::size_t requested) noexcept {
    return capacity >= requested;
}

} // namespace Vulkan::StagingBufferPoolUtils
