// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* virtual_buffer.h */
// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <bit>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#include <sys/mman.h>
#endif

#include "common/alignment.h"
#include "common/assert.h"

namespace Common {

#ifdef _WIN32
constexpr u64 HostPageSize = 0x1000;
constexpr u64 HostPageBits = 12;
constexpr u64 HostPageMask = ~(HostPageSize - 1);
bool CommitVectorPage(uintptr_t addr, bool write) noexcept;
#else
inline const u64 HostPageSize = sysconf(_SC_PAGESIZE);
inline const u64 HostPageBits = std::countr_zero(HostPageSize);
inline const u64 HostPageMask = ~(HostPageSize - 1);
#endif

void* AllocateMemoryPages(std::size_t size) noexcept;
void FreeMemoryPages(void* base, std::size_t size) noexcept;
void DecommitVectorPage(uintptr_t base) noexcept;

/// A large page-aligned buffer that has optimized memory usage for zero-writes.
template <typename T>
    // MSVC doesn't regard structs with atomics as trivially copyable
    // requires std::is_trivially_copyable_v<T>
class SparseLargeVector final {
public:
    constexpr SparseLargeVector() = default;

    explicit SparseLargeVector(std::size_t count)
        : alloc_size{CheckedByteSize(count)}
        , committed_pages(MetadataPageCount(alloc_size))
    {
        base_ptr = static_cast<T*>(AllocateMemoryPages(alloc_size));
    }

    ~SparseLargeVector() noexcept {
        FreeMemoryPages(base_ptr, alloc_size);
    }

    SparseLargeVector(const SparseLargeVector&) = delete;
    SparseLargeVector& operator=(const SparseLargeVector&) = delete;
    SparseLargeVector(SparseLargeVector&& other) = delete;
    SparseLargeVector& operator=(SparseLargeVector&& other) = delete;

    void ResizeAndClear(std::size_t count) {
        const std::size_t new_size = CheckedByteSize(count);
        if (new_size != alloc_size) {
            // Construct metadata before releasing the current allocation. A vector
            // allocation can throw, so keep the existing object intact until every
            // replacement resource has been prepared.
            auto new_committed_pages = MakeMetadata(new_size);
            T* new_base_ptr = static_cast<T*>(AllocateMemoryPages(new_size));
            T* old_base_ptr = base_ptr;
            const std::size_t old_size = alloc_size;

            base_ptr = new_base_ptr;
            alloc_size = new_size;
            committed_pages.swap(new_committed_pages);
            FreeMemoryPages(old_base_ptr, old_size);
        }
    }

    /// Returns a reference to the value of the requested index and allocates memory if needed.
    T& GetAndFault(std::size_t index) noexcept {
        if (index >= size()) {
            UNREACHABLE_MSG("Out of bounds RW access on SparseLargeVector @ {}", index);
        }

        if (!IsCommittedPage(index) && !CommitPage(index)) {
            UNREACHABLE_MSG("Cannot access SparseLargeVector index {} with RW permission", index);
        }
        return base_ptr[index];
    }

    /// Returns a reference to the value of the requested index if initialized, or will otherwise return a zero-initialized object.
    const T& GetOrDefault(std::size_t index) const {
        if (index >= size() || base_ptr == nullptr) {
            UNREACHABLE_MSG("Out of bounds read on SparseLargeVector @ {} (size {})", index, size());
        }
#ifdef _WIN32
        if (!IsCommittedPage(index)) {
            return *reinterpret_cast<const T*>(&default_val);
        }
#endif
        // On non-Windows, OS page table should optimize this by pointing to a zero page if unallocated.
        return base_ptr[index];
    }

    void Set(std::size_t index, const T& value) noexcept {
        if (index >= size()) {
            LOG_CRITICAL(Common_Memory, "Out of bounds write on SparseLargeVector @ {}", index);
            return;
        }
        if (IsCommittedPage(index) || CommitPage(index)) {
            base_ptr[index] = value;
        } else {
            LOG_CRITICAL(Common_Memory, "Failed to commit SparseLargeVector index {}", index);
        }
    }

    void ZeroRegion(std::size_t start, std::size_t end_) noexcept {
        if (start > end_ || end_ > size() || (size() != 0 && base_ptr == nullptr)) {
            UNREACHABLE_MSG("Invalid SparseLargeVector zero range [{}, {}) for size {}", start,
                            end_, size());
        }
        if (start == end_) {
            return;
        }
        u64 base = reinterpret_cast<u64>(&base_ptr[start]);
        const u64 end = reinterpret_cast<u64>(&base_ptr[end_]);

        const u64 end_page = AlignUp(base, HostPageSize);
        const u64 first_size = (std::min)(end_page, end) - base;

        if (IsCommittedPage(start)) {
            std::memset(reinterpret_cast<void*>(base), 0, first_size);
        }

        if (end <= end_page)
            return;

        base = end_page;

        for (u64 page = base; page < end; page += HostPageSize) {
            auto index = (page - reinterpret_cast<u64>(base_ptr)) / sizeof(T);
            if (!IsCommittedPage(index)) {
                continue;
            }

            if (end - page >= HostPageSize) {
                DecommitPage(index);
            } else {
                std::memset(reinterpret_cast<void*>(page), 0, end - page);
            }
        }
    }

    [[nodiscard]] bool CommitRegion(size_t index, size_t end_) {
        if (index > end_ || end_ > size() || (size() != 0 && base_ptr == nullptr)) {
            LOG_ERROR(Common_Memory, "Invalid SparseLargeVector commit range [{}, {}) for size {}",
                      index, end_, size());
            return false;
        }
        const u64 base = static_cast<u64>(index) * sizeof(T);
        const u64 end = static_cast<u64>(end_) * sizeof(T);

        for (u64 page = AlignDown(base, HostPageSize); page < end; page += HostPageSize) {
            if (!IsCommittedPage(page / sizeof(T))) {
                if (!CommitPage(page / sizeof(T))) {
                    return false;
                }
            }
        }
        return true;
    }

    T& GetUnchecked(size_t index) {
        if (index >= size()) {
            UNREACHABLE_MSG("Out of bounds SparseLargeVector access @ {} (size {})", index, size());
        }
        // The caller must have successfully committed this range first. Keeping the
        // committed-page bitmap out of this per-entry hot path avoids an extra atomic
        // load for every guest 4 KiB page mapped.
        return base_ptr[index];
    }

    [[nodiscard]] constexpr const T& operator[](std::size_t index) const noexcept {
        return GetOrDefault(index);
    }

    [[nodiscard]] constexpr const T* data() const noexcept {
        return base_ptr;
    }

    [[nodiscard]] constexpr std::size_t size() const noexcept {
        return alloc_size / sizeof(T);
    }

private:
    static std::size_t MetadataPageCount(std::size_t byte_size) noexcept {
        const std::size_t pages_per_group = HostPageSize * 64;
        return byte_size / pages_per_group + (byte_size % pages_per_group != 0);
    }

    static std::vector<std::atomic<u64>> MakeMetadata(std::size_t byte_size) {
        return std::vector<std::atomic<u64>>(MetadataPageCount(byte_size));
    }

    static std::size_t CheckedByteSize(std::size_t count) {
        if (count > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
            throw std::length_error("SparseLargeVector size overflows size_t");
        }
        return count * sizeof(T);
    }

    [[nodiscard]] constexpr bool IsCommittedPage(std::size_t index) const noexcept {
        if (index >= size()) {
            LOG_CRITICAL(Common_Memory, "Out of bounds access on large vector @ {}", index);
            return false;
        }

        auto page = (index * sizeof(T)) >> HostPageBits;
        if ((page >> 6) >= committed_pages.size()) {
            LOG_CRITICAL(Common_Memory, "Out of bounds commit metadata access on large vector @ {}",
                         index);
            return false;
        }
        auto val = committed_pages[page >> 6].load(std::memory_order_acquire);
        return (val >> (page & 63)) & 1;
    }

    constexpr bool CommitPage(std::size_t index) noexcept {
        if (index >= size() || base_ptr == nullptr) {
            return false;
        }
        auto page_index = (index * sizeof(T)) >> HostPageBits;
        if ((page_index >> 6) >= committed_pages.size()) {
            return false;
        }
        auto page = reinterpret_cast<uintptr_t>(base_ptr + index) & HostPageMask;
#if defined(_WIN32)
        if (!CommitVectorPage(page, true)) {
            return false;
        }
#else
        if (mprotect(reinterpret_cast<void*>(page), HostPageSize, PROT_READ | PROT_WRITE) != 0) {
            LOG_ERROR(Common_Memory, "Failed to commit large buffer region at index {}, error {}", index, strerror(errno));
            return false;
        }
#endif

        committed_pages[page_index >> 6].fetch_or(1ULL << (page_index & 63), std::memory_order_release);
        return true;
    }

    constexpr void DecommitPage(std::size_t index) noexcept {
        auto page_index = (index * sizeof(T)) >> HostPageBits;
        auto page = reinterpret_cast<uintptr_t>(base_ptr + index) & HostPageMask;

        committed_pages[page_index >> 6].fetch_and(~(1ULL << (page_index & 63)), std::memory_order_release);
        DecommitVectorPage(page);
    }

    std::size_t alloc_size{};
    T* base_ptr{};

    std::vector<std::atomic<u64>> committed_pages{};
#ifdef _WIN32
    const std::array<u8, sizeof(T)> default_val{};
#endif
};

} // namespace Common
