#pragma once
#include <cerrno>
#include <signal.h>
#include <sys/syscall.h>
#include <ucontext.h>

namespace steamohos::fex {
// OHOS can trap optional file-handle / NUMA / robust-list queries instead of returning
// Linux's unsupported-capability errno. FEX's inline SVC bypasses libc hooks.
// Resume only this explicit allowlist after a genuine host seccomp trap;
// never report success, touch output buffers or consume unrelated faults.
inline bool UnsupportedSandboxCall(const siginfo_t* info, void* context) noexcept {
    if (!info || !context || info->si_code != 1 /* SYS_SECCOMP */) return false;
#if defined(__aarch64__)
    if (info->si_arch != 0xc00000b7u) return false; // AUDIT_ARCH_AARCH64
#elif defined(__x86_64__)
    if (info->si_arch != 0xc000003eu) return false; // AUDIT_ARCH_X86_64
#else
    return false;
#endif
    switch (info->si_syscall) {
        case SYS_name_to_handle_at:
        case SYS_open_by_handle_at:
        case SYS_get_mempolicy:
        case SYS_set_mempolicy:
        case SYS_mbind:
        case SYS_migrate_pages:
        case SYS_move_pages:
        // Query failure must remain observable; set_robust_list and futex
        // semantics are untouched. Never fabricate a host or guest list.
        case SYS_get_robust_list:
            break;
        default: return false;
    }
    auto* state = static_cast<ucontext_t*>(context);
#if defined(__aarch64__)
    state->uc_mcontext.regs[0] = static_cast<unsigned long>(-ENOSYS);
#elif defined(__x86_64__)
    state->uc_mcontext.gregs[REG_RAX] = -ENOSYS;
#endif
    // A seccomp signal resumes after the syscall instruction. Preserve PC,
    // all other registers and every caller-provided buffer.
    return true;
}
}
