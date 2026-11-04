"""Keep GNU Wine optional sandbox probes on their real ENOSYS fallbacks."""
def proton_syscall_assembly(assembly):
    head, marker, tail = assembly.partition('#elif defined(__x86_64__)')
    if not marker or head.count('    b.eq SteamOhosFexOptionalCall') != 2:
        raise ValueError('GNU Proton optional syscall input changed; audit first-match dispatch')
    # The common adapter recently gained earlier FEX-only 449/282 branches.
    # Those must not intercept Wine's later fallbacks and forward into seccomp.
    head=head.replace('    b.eq SteamOhosFexOptionalCall','    b.eq SteamOhosSyscallUnavailable')
    if head.count('syscall:\n') != 1:raise ValueError('Missing ARM64 syscall entry')
    head=head.replace('syscall:\n','syscall:\n    cmp x0, #374 // additional userfaultfd probe in this GE build\n    b.eq SteamOhosSyscallUnavailable\n',1)
    return head+marker+tail
