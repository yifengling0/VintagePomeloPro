// Preserve all syscall argument registers when forwarding the variadic ABI.
#include <asm/unistd.h>
// Linux UAPI number, also for GNU sysroots predating Landlock.
#ifndef __NR_landlock_create_ruleset
#define __NR_landlock_create_ruleset 444
#endif
#ifndef __NR_close_range
#define __NR_close_range 436
#endif
#ifndef __NR_openat2
#define __NR_openat2 437
#endif
.text
.global syscall
.type syscall, %function
.hidden SteamOhosNextSyscall
.hidden SteamOhosLandlockCreateRuleset
.hidden SteamOhosOpenAt2
.hidden SteamOhosCloseRange
.hidden SteamOhosQueryRobustList
.hidden SteamOhosSyscallUnavailable
#if defined(__aarch64__)
syscall:
    cmp x0, #__NR_openat2
    b.eq 5f
    cmp x0, #__NR_get_robust_list
    b.eq 1f
    cmp x0, #__NR_landlock_create_ruleset
    b.eq 3f
    cmp x0, #__NR_close_range
    b.eq 4f
    adrp x16, :got:SteamOhosNextSyscall
    ldr x16, [x16, :got_lo12:SteamOhosNextSyscall]
    ldr x16, [x16]
    cbz x16, 2f
    br x16
1:
    mov x0, x1
    mov x1, x2
    mov x2, x3
    b SteamOhosQueryRobustList
3:
    mov x0, x1
    mov x1, x2
    mov x2, x3
    b SteamOhosLandlockCreateRuleset
4:
    mov x0, x1
    mov x1, x2
    mov x2, x3
    b SteamOhosCloseRange
5:
    mov x0, x1
    mov x1, x2
    mov x2, x3
    mov x3, x4
    b SteamOhosOpenAt2
2:
    b SteamOhosSyscallUnavailable
#elif defined(__x86_64__)
syscall:
    cmp $__NR_openat2, %rdi
    je 5f
    cmp $__NR_get_robust_list, %rdi
    je 1f
    cmp $__NR_landlock_create_ruleset, %rdi
    je 3f
    cmp $__NR_close_range, %rdi
    je 4f
    mov SteamOhosNextSyscall(%rip), %r11
    test %r11, %r11
    je 2f
    jmp *%r11
1:
    mov %rsi, %rdi
    mov %rdx, %rsi
    mov %rcx, %rdx
    jmp SteamOhosQueryRobustList
3:
    mov %rsi, %rdi
    mov %rdx, %rsi
    mov %rcx, %rdx
    jmp SteamOhosLandlockCreateRuleset
4:
    mov %rsi, %rdi
    mov %rdx, %rsi
    mov %rcx, %rdx
    jmp SteamOhosCloseRange
5:
    mov %rsi, %rdi
    mov %rdx, %rsi
    mov %rcx, %rdx
    mov %r8, %rcx
    jmp SteamOhosOpenAt2
2:
    jmp SteamOhosSyscallUnavailable
#else
#error Unsupported GNU-native syscall ABI
#endif
.size syscall, .-syscall
.section .note.GNU-stack,"",%progbits
