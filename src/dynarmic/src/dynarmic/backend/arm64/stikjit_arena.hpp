// SPDX-License-Identifier: GPL-3.0-or-later
// iOS-only JIT code cache: RW/RX views of one debugger-prepared arena.
#pragma once

#include <cstddef>
#include <cstdint>
#include <new>

namespace Dynarmic::Backend::Arm64::StikJIT {

class CodeBlock final {
public:
    explicit CodeBlock(std::size_t bytes);
    ~CodeBlock();
    CodeBlock(const CodeBlock&) = delete;
    CodeBlock& operator=(const CodeBlock&) = delete;
    CodeBlock(CodeBlock&&) = delete;
    CodeBlock& operator=(CodeBlock&&) = delete;

    // Executable address for branch targets, traps and native entry points.
    std::uint32_t* ptr() const noexcept { return xptr(); }
    std::uint32_t* xptr() const noexcept { return exec_; }
    // Writable alias of the same physical pages, never used as a branch target.
    std::uint32_t* wptr() const noexcept { return write_; }

    void protect() noexcept {}   // W^X permanently handled by the two aliases.
    void unprotect() noexcept {} // Do NOT mprotect the RX mapping.
    void invalidate(std::uint32_t* exec_addr, std::size_t bytes);
    void invalidate_all() { invalidate(xptr(), bytes_); }

private:
    std::uint32_t* exec_ = nullptr;
    std::uint32_t* write_ = nullptr;
    std::size_t bytes_ = 0;
    std::size_t allocation_offset_ = 0;
    std::size_t allocation_size_ = 0;
};

} // namespace Dynarmic::Backend::Arm64::StikJIT

// C ABI exposed by the static Dynarmic library to the Swift/Obj-C host.
// This is an EXPERIMENTAL manual protocol: the user must explicitly choose
// universal.js in StikDebug and return to Eden before calling prepare.
extern "C" bool eden_stikjit_prepare_arena(void);
extern "C" bool eden_stikjit_arena_ready(void);
extern "C" bool eden_stikjit_execution_test_passed(void);
extern "C" bool eden_stikjit_run_execution_test(void);
extern "C" const char* eden_stikjit_status(void);
