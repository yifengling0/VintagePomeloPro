#pragma once
#include <cstdint>
#include <limits>
#include <signal.h>

namespace steamohos::fex {
// The sigaltstack query describes the handler's current stack. uc_stack must
// describe the interrupted context, including whether it was already on-stack.
// Do not guess for disabled/autodisarmed, partial or overflowing stack records.
inline bool RepairEmptySignalStack(stack_t& saved,const stack_t& registered,
                                   uintptr_t interruptedSp) noexcept {
    if(saved.ss_sp||saved.ss_size||saved.ss_flags||registered.ss_flags!=SS_ONSTACK||
       !registered.ss_sp||!registered.ss_size)return false;
    const auto begin=reinterpret_cast<uintptr_t>(registered.ss_sp);
    if(registered.ss_size>std::numeric_limits<uintptr_t>::max()-begin)return false;
    const auto end=begin+registered.ss_size;
    saved.ss_sp=registered.ss_sp;
    saved.ss_size=registered.ss_size;
    saved.ss_flags=interruptedSp>=begin&&interruptedSp<end?SS_ONSTACK:0;
    return true;
}
}
