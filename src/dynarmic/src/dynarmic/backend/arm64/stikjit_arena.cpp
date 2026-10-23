// SPDX-License-Identifier: GPL-3.0-or-later
#include "dynarmic/backend/arm64/stikjit_arena.hpp"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(__APPLE__) && TARGET_OS_IPHONE && defined(__aarch64__)

#include <mach/mach.h>
#include <mach/vm_map.h>
#include <libkern/OSCacheControl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdio>
#include <map>
#include <mutex>
#include <stdexcept>

#include "dynarmic/backend/arm64/stikjit_universal.hpp"

namespace Dynarmic::Backend::Arm64::StikJIT {
namespace {
// Single preallocated arena: prepare once while universal.js is servicing BRK;
// then suballocate all Dynarmic code-cache blocks without another debugger trap.
// Cache sizes in this Dynarmic fork are capped at 128 MiB per AddressSpace.
constexpr std::size_t kArenaBytes = 256u * 1024u * 1024u;
constexpr std::size_t kAlignment = 16u * 1024u; // iOS arm64 page alignment
std::mutex g_mutex;
std::byte* g_exec = nullptr;
std::byte* g_write = nullptr;
std::map<std::size_t, std::size_t> g_free_ranges;
bool g_tried = false;
bool g_ready = false;
bool g_execution_test_passed = false;
bool g_allocations_started = false;
char g_status[256] = "StikJIT: not prepared";

std::size_t AlignUp(std::size_t value) {
    if (value > SIZE_MAX - (kAlignment - 1)) throw std::bad_alloc{};
    return (value + kAlignment - 1) & ~(kAlignment - 1);
}

bool LooksLikeRxPointer(void* ptr) {
    const auto value = reinterpret_cast<std::uintptr_t>(ptr);
    return value >= kAlignment && value != UINTPTR_MAX && (value % kAlignment) == 0;
}

bool PrepareLocked() {
    if (g_tried) return g_ready;

    if (!DebuggerAttached()) {
        std::snprintf(g_status, sizeof(g_status), "StikJIT: CS_DEBUGGED absent; no breakpoint called");
        return false;
    }
    g_tried = true;

    // UNIVERSAL.JS MUST ALREADY BE SERVICING THE DEBUG CONNECTION.
    // CS_DEBUGGED alone cannot guarantee this; the UI requires explicit action.
    // Passing nullptr is required on TXM/SPTM devices (debugger allocates RX).
    void* rx = nullptr;
    try {
        rx = PrepareRegion(nullptr, kArenaBytes, true);
    } catch (const std::exception& ex) {
        std::snprintf(g_status, sizeof(g_status), "StikJIT: prepare failed: %.160s", ex.what());
        return false;
    }
    if (!LooksLikeRxPointer(rx)) {
        std::snprintf(g_status, sizeof(g_status), "StikJIT: invalid RX address: %p", rx);
        return false;
    }

    vm_address_t alias = 0;
    vm_prot_t cur = VM_PROT_NONE, max = VM_PROT_NONE;
    const auto remap = vm_remap(mach_task_self(), &alias,
        static_cast<vm_size_t>(kArenaBytes), 0, VM_FLAGS_ANYWHERE,
        mach_task_self(), reinterpret_cast<vm_address_t>(rx), FALSE,
        &cur, &max, VM_INHERIT_NONE);
    if (remap != KERN_SUCCESS) {
        std::snprintf(g_status, sizeof(g_status), "StikJIT: vm_remap failed kr=%d", int(remap));
        return false;
    }
    const auto protection = vm_protect(mach_task_self(), alias,
        static_cast<vm_size_t>(kArenaBytes), FALSE,
        VM_PROT_READ | VM_PROT_WRITE);
    if (protection != KERN_SUCCESS) {
        vm_deallocate(mach_task_self(), alias, static_cast<vm_size_t>(kArenaBytes));
        std::snprintf(g_status, sizeof(g_status), "StikJIT: RW alias vm_protect failed kr=%d", int(protection));
        return false;
    }

    // Never call detach until BOTH sides are mapped. The universal script must
    // acknowledge the detach breakpoint; only then is the arena marked ready.
    try {
        Detach(true);
    } catch (const std::exception& ex) {
        vm_deallocate(mach_task_self(), alias, static_cast<vm_size_t>(kArenaBytes));
        std::snprintf(g_status, sizeof(g_status), "StikJIT: detach failed: %.160s", ex.what());
        return false;
    }

    g_exec = static_cast<std::byte*>(rx);
    g_write = reinterpret_cast<std::byte*>(alias);
    g_free_ranges.clear();
    g_free_ranges.emplace(0, kArenaBytes);
    g_ready = true;
    std::snprintf(g_status, sizeof(g_status), "StikJIT: arena prepared: %zu MiB, rx=%p rw=%p",
        kArenaBytes >> 20, rx, reinterpret_cast<void*>(alias));
    return true;
}
} // namespace

CodeBlock::CodeBlock(std::size_t bytes) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_ready) throw std::runtime_error("StikJIT: code cache requested before arena preparation");
    if (!bytes) throw std::bad_alloc{};
    const std::size_t allocation = AlignUp(bytes);
    for (auto it = g_free_ranges.begin(); it != g_free_ranges.end(); ++it) {
        if (it->second < allocation) continue;
        allocation_offset_ = it->first;
        allocation_size_ = allocation;
        g_allocations_started = true;
        const std::size_t remainder = it->second - allocation;
        g_free_ranges.erase(it);
        if (remainder) g_free_ranges.emplace(allocation_offset_ + allocation, remainder);
        exec_ = reinterpret_cast<std::uint32_t*>(g_exec + allocation_offset_);
        write_ = reinterpret_cast<std::uint32_t*>(g_write + allocation_offset_);
        bytes_ = bytes;
        return;
    }
    std::size_t free_bytes = 0;
    for (const auto& [offset, size] : g_free_ranges) free_bytes += size;
    std::snprintf(g_status, sizeof(g_status),
        "StikJIT: arena exhausted requesting %zu MiB; %zu MiB free across %zu ranges",
        allocation >> 20, free_bytes >> 20, g_free_ranges.size());
    throw std::bad_alloc{};
}

CodeBlock::~CodeBlock() {
    if (!allocation_size_) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto next = g_free_ranges.lower_bound(allocation_offset_);
    auto previous = next;
    if (previous != g_free_ranges.begin()) {
        --previous;
        if (previous->first + previous->second == allocation_offset_) {
            allocation_offset_ = previous->first;
            allocation_size_ += previous->second;
            g_free_ranges.erase(previous);
        }
    }
    next = g_free_ranges.lower_bound(allocation_offset_);
    if (next != g_free_ranges.end() && allocation_offset_ + allocation_size_ == next->first) {
        allocation_size_ += next->second;
        g_free_ranges.erase(next);
    }
    g_free_ranges.emplace(allocation_offset_, allocation_size_);
    allocation_size_ = 0;
}

void CodeBlock::invalidate(std::uint32_t* exec_addr, std::size_t bytes) {
    if (!exec_addr || !bytes) return;
    const auto address = reinterpret_cast<std::uintptr_t>(exec_addr);
    const auto base = reinterpret_cast<std::uintptr_t>(exec_);
    if (address < base || address - base > bytes_ || bytes > bytes_ - (address - base)) return;
    auto* const write_addr = reinterpret_cast<void*>(
        reinterpret_cast<std::uintptr_t>(write_) + (address - base));
    // Stores go through RW and fetches through RX: clean the data side at the
    // address that received stores, then invalidate instructions at the RX address.
    sys_dcache_flush(write_addr, bytes);
    sys_icache_invalidate(exec_addr, bytes);
}

bool IsReady() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_ready;
}

bool ExecutionTestPassed() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_execution_test_passed;
}

bool RunExecutionTest() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_ready) {
        std::snprintf(g_status, sizeof(g_status), "StikJIT: execution test refused; arena not prepared");
        return false;
    }
    if (g_allocations_started) {
        std::snprintf(g_status, sizeof(g_status), "StikJIT: execution test must run before Dynarmic allocates code caches");
        return false;
    }
    if (g_execution_test_passed) return true;

    // `mov w0, #0x51a7; ret` (AArch64). Write only through RW, flush the executable
    // alias, then call RX. This is the first point where Eden proves native execution.
    auto* const write = reinterpret_cast<std::uint32_t*>(g_write);
    auto* const exec = reinterpret_cast<std::uint32_t*>(g_exec);
    write[0] = 0x528A34E0u;
    write[1] = 0xD65F03C0u;
    sys_dcache_flush(write, 2 * sizeof(std::uint32_t));
    sys_icache_invalidate(exec, 2 * sizeof(std::uint32_t));
    using TestFunction = std::uint32_t (*)();
    const auto function = reinterpret_cast<TestFunction>(exec);
    const std::uint32_t result = function();
    g_execution_test_passed = result == 0x51A7u;
    std::snprintf(g_status, sizeof(g_status),
        "StikJIT: RX/RW ARM64 test %s (returned 0x%04x)",
        g_execution_test_passed ? "passed" : "failed", result);
    return g_execution_test_passed;
}

bool Initialize() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return PrepareLocked();
}

const char* LastStatus() {
    static thread_local char snapshot[sizeof(g_status)];
    std::lock_guard<std::mutex> lock(g_mutex);
    std::snprintf(snapshot, sizeof(snapshot), "%s", g_status);
    return snapshot;
}
} // namespace Dynarmic::Backend::Arm64::StikJIT

extern "C" bool eden_stikjit_prepare_arena(void) {
    return Dynarmic::Backend::Arm64::StikJIT::Initialize();
}
extern "C" bool eden_stikjit_arena_ready(void) {
    return Dynarmic::Backend::Arm64::StikJIT::IsReady();
}
extern "C" bool eden_stikjit_execution_test_passed(void) {
    return Dynarmic::Backend::Arm64::StikJIT::ExecutionTestPassed();
}
extern "C" bool eden_stikjit_run_execution_test(void) {
    return Dynarmic::Backend::Arm64::StikJIT::RunExecutionTest();
}
extern "C" const char* eden_stikjit_status(void) {
    return Dynarmic::Backend::Arm64::StikJIT::LastStatus();
}

#endif // iPhone arm64
