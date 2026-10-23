/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include <mutex>

#include <oaknut/code_block.hpp>
#include <oaknut/oaknut.hpp>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if defined(__APPLE__) && TARGET_OS_IPHONE
#include "dynarmic/backend/arm64/stikjit_arena.hpp"
#endif

#include "dynarmic/backend/arm64/abi.h"
#include "dynarmic/common/spin_lock.h"

namespace Dynarmic {

using Backend::Arm64::Wscratch0;
using Backend::Arm64::Wscratch1;
using namespace oaknut::util;

void EmitSpinLockLock(oaknut::CodeGenerator& code, oaknut::XReg ptr) {
    oaknut::Label start, loop;

    code.MOV(Wscratch1, 1);
    code.SEVL();
    code.l(start);
    code.WFE();
    code.l(loop);
    code.LDAXR(Wscratch0, ptr);
    code.CBNZ(Wscratch0, start);
    code.STXR(Wscratch0, Wscratch1, ptr);
    code.CBNZ(Wscratch0, loop);
}

void EmitSpinLockUnlock(oaknut::CodeGenerator& code, oaknut::XReg ptr) {
    code.STLR(WZR, ptr);
}

namespace {

struct SpinLockImpl {
    SpinLockImpl();

    void Initialize();

#if defined(__APPLE__) && TARGET_OS_IPHONE
    Backend::Arm64::StikJIT::CodeBlock mem;
#else
    oaknut::CodeBlock mem;
#endif
    oaknut::CodeGenerator code;

    void (*lock)(volatile int*);
    void (*unlock)(volatile int*);
};

std::once_flag flag;

// Constructed on first use rather than at namespace scope. On iOS its generated code
// uses the prepared StikJIT arena; other platforms use oaknut's native CodeBlock.
// Deferring construction also avoids allocating executable memory during dyld startup.
//
// Function-local statics are initialised thread-safely since C++11, so this does not need
// its own guard, and it also removes a static-initialisation-order dependency.
SpinLockImpl& Impl() noexcept {
    static SpinLockImpl impl;
    return impl;
}

SpinLockImpl::SpinLockImpl()
#if defined(__APPLE__) && TARGET_OS_IPHONE
        : mem{4096}
        , code{mem.wptr(), mem.xptr()} {}
#else
        : mem{4096}
        , code{mem.ptr(), mem.ptr()} {}
#endif

void SpinLockImpl::Initialize() {
    mem.unprotect();

    lock = code.xptr<void (*)(volatile int*)>();
    EmitSpinLockLock(code, X0);
    code.RET();

    unlock = code.xptr<void (*)(volatile int*)>();
    EmitSpinLockUnlock(code, X0);
    code.RET();

    mem.protect();
    mem.invalidate_all();
}

}  // namespace

void SpinLock::Lock() noexcept {
    SpinLockImpl& impl = Impl();
    std::call_once(flag, &SpinLockImpl::Initialize, impl);
    impl.lock(&storage);
}

void SpinLock::Unlock() noexcept {
    SpinLockImpl& impl = Impl();
    std::call_once(flag, &SpinLockImpl::Initialize, impl);
    impl.unlock(&storage);
}

}  // namespace Dynarmic
