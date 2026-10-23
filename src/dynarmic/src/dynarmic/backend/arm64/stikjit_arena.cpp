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
std::size_t g_offset = 0;
bool g_tried = false;
bool g_ready = false;
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
    g_ready = true;
    std::snprintf(g_status, sizeof(g_status), "StikJIT: arena prepared: %zu MiB, rx=%p rw=%p",
        kArenaBytes >> 20, rx, reinterpret_cast<void*>(alias));
    return true;
}
} // namespace

CodeBlock::CodeBlock(std::size_t bytes) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_ready) throw std::runtime_error("StikJIT: code cache requested before arena preparation");
    const std::size_t allocation = AlignUp(bytes);
    if (!bytes || allocation > kArenaBytes - g_offset) {
        throw std::bad_alloc{};
    }
    exec_ = reinterpret_cast<std::uint32_t*>(g_exec + g_offset);
    write_ = reinterpret_cast<std::uint32_t*>(g_write + g_offset);
    bytes_ = bytes;
    g_offset += allocation;
}

CodeBlock::~CodeBlock() = default; // Arena retained for the process lifetime.

void CodeBlock::invalidate(std::uint32_t* exec_addr, std::size_t bytes) {
    if (exec_addr && bytes) sys_icache_invalidate(exec_addr, bytes);
}

bool IsReady() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_ready;
}

bool Initialize() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return PrepareLocked();
}

const char* LastStatus() { return g_status; } // immutable after Initialize returns
} // namespace Dynarmic::Backend::Arm64::StikJIT

extern "C" bool eden_stikjit_prepare_arena(void) {
    return Dynarmic::Backend::Arm64::StikJIT::Initialize();
}
extern "C" bool eden_stikjit_arena_ready(void) {
    return Dynarmic::Backend::Arm64::StikJIT::IsReady();
}
extern "C" const char* eden_stikjit_status(void) {
    return Dynarmic::Backend::Arm64::StikJIT::LastStatus();
}

#endif // iPhone arm64
