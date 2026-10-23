// SPDX-License-Identifier: GPL-3.0-or-later
// StikDebug universal.js protocol. This file deliberately does NOT allocate
// writable aliases or mark JIT ready. It is a building block for the allocator.
#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(__APPLE__) && TARGET_OS_IPHONE && defined(__aarch64__)
#include <cerrno>
#include <sys/syscall.h>
#include <unistd.h>

namespace Dynarmic::Backend::Arm64::StikJIT {

// Apple's CS_DEBUGGED flag, distinct from execute permission.
constexpr std::uint32_t CS_DEBUGGED_FLAG = 0x10000000u;

inline bool DebuggerAttached() noexcept {
    std::uint32_t flags = 0;
    // CS_OPS_STATUS = 0. csops is private API; only for sideloaded builds.
    return syscall(SYS_csops, getpid(), 0, &flags, sizeof(flags)) == 0 &&
           (flags & CS_DEBUGGED_FLAG) != 0;
}

// Keep these naked: x0 and x1 must be passed to the debugger unchanged.
__attribute__((noinline, optnone, naked))
inline void* BreakpointPrepareRegion(void* address, std::size_t length) {
    __asm__ volatile("mov x16, #1\n"
                     "brk #0xf00d\n"
                     "ret\n");
}

__attribute__((noinline, optnone, naked))
inline void BreakpointDetach() {
    __asm__ volatile("mov x16, #0\n"
                     "brk #0xf00d\n"
                     "ret\n");
}

// Never call BRK unless StikDebug with universal.js is actively servicing
// the process. CS_DEBUGGED is only a necessary condition, not sufficient.
inline void* PrepareRegion(void* address, std::size_t length,
                           bool universal_script_confirmed) {
    if (length == 0 || !universal_script_confirmed || !DebuggerAttached()) {
        throw std::runtime_error("StikJIT universal debugger not confirmed");
    }
    void* result = BreakpointPrepareRegion(address, length);
    if (!result) {
        throw std::runtime_error("StikJIT failed to prepare executable region");
    }
    return result;
}

inline void Detach(bool universal_script_confirmed) {
    if (!universal_script_confirmed || !DebuggerAttached()) {
        throw std::runtime_error("StikJIT detach without confirmed debugger");
    }
    BreakpointDetach();
}

} // namespace Dynarmic::Backend::Arm64::StikJIT
#endif
