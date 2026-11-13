/* SPDX-License-Identifier: MIT
 * GNU x86-64 diagnostic syscall interposer. Observe only exit/exit_group;
 * all other requests retain the kernel ABI, result and libc errno contract.
 */
.text
.globl syscall
.type syscall,@function
syscall:
    .cfi_startproc
    cmp $60,%rdi
    je .Lobserve
    cmp $231,%rdi
    jne .Lraw
.Lobserve:
    sub $24,%rsp
    .cfi_adjust_cfa_offset 24
    mov %rdi,(%rsp)
    mov %rsi,8(%rsp)
    call SteamOhosObserveGuestExit
    mov (%rsp),%rdi
    mov 8(%rsp),%rsi
    add $24,%rsp
    .cfi_adjust_cfa_offset -24
.Lraw:
    mov %rdi,%rax
    mov %rsi,%rdi
    mov %rdx,%rsi
    mov %rcx,%rdx
    mov %r8,%r10
    mov %r9,%r8
    mov 8(%rsp),%r9
    syscall
    cmp $-4095,%rax
    jae .Lerror
    ret
.Lerror:
    push %rax
    .cfi_adjust_cfa_offset 8
    call __errno_location@PLT
    pop %rdx
    .cfi_adjust_cfa_offset -8
    neg %edx
    mov %edx,(%rax)
    or $-1,%rax
    ret
    .cfi_endproc
.size syscall,.-syscall
.section .note.GNU-stack,"",@progbits
