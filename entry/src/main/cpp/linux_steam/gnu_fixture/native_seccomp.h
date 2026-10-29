#pragma once
#include <cerrno>
#include <signal.h>
#include <sys/syscall.h>
#include <ucontext.h>

namespace steamohos::fex {
// OHOS traps unavailable file-handle facilities instead of returning a Linux
// unsupported-capability errno. An inline FEX SVC bypasses libc interposition.
// Only these two optional operations may resume; never swallow other faults.
inline bool UnsupportedFileHandleCall(const siginfo_t* info, void* context) noexcept {
    if (!info || !context || info->si_code != 1 /* SYS_SECCOMP */) return false;
#if defined(__aarch64__)
    if (info->si_arch != 0xc00000b7u) return false; // AUDIT_ARCH_AARCH64
#elif defined(__x86_64__)
    if (info->si_arch != 0xc000003eu) return false; // AUDIT_ARCH_X86_64
#else
    return false;
#endif
    if (info->si_syscall != SYS_name_to_handle_at && info->si_syscall != SYS_open_by_handle_at)
        return false;
    auto* state = static_cast<ucontext_t*>(context);
#if defined(__aarch64__)
    state->uc_mcontext.regs[0] = static_cast<unsigned long>(-ENOSYS);
#elif defined(__x86_64__)
    state->uc_mcontext.gregs[REG_RAX] = -ENOSYS;
#endif
    // A seccomp signal resumes after the syscall instruction. Preserve PC,
    // all other registers and the caller's file-handle/mount-id buffers.
    return true;
}
}
