#include <asm/unistd.h>
.text
.hidden SteamOhosGuestNextSyscall
.hidden SteamOhosGuestResolveSyscall
.hidden SteamOhosGuestQueryRobust
.hidden SteamOhosGuestSetRobust
.globl syscall
.type syscall,@function
syscall:
    cmp $__NR_get_robust_list,%rdi
    je SteamOhosGuestQueryRobust
    cmp $__NR_set_robust_list,%rdi
    je SteamOhosGuestSetRobust
    mov SteamOhosGuestNextSyscall(%rip),%rax
    test %rax,%rax
    je .Lresolve_guest_syscall
    jmp *%rax
.Lresolve_guest_syscall:
    // Preserve all six register arguments and the caller's seventh stack
    // argument. Every unrelated syscall tail-calls the real libc ABI.
    sub $56,%rsp
    mov %rdi,0(%rsp)
    mov %rsi,8(%rsp)
    mov %rdx,16(%rsp)
    mov %rcx,24(%rsp)
    mov %r8,32(%rsp)
    mov %r9,40(%rsp)
    call SteamOhosGuestResolveSyscall
    mov 0(%rsp),%rdi
    mov 8(%rsp),%rsi
    mov 16(%rsp),%rdx
    mov 24(%rsp),%rcx
    mov 32(%rsp),%r8
    mov 40(%rsp),%r9
    add $56,%rsp
    jmp *%rax
.size syscall,.-syscall
.section .note.GNU-stack,"",@progbits
