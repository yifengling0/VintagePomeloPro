.text
.p2align 4
.global SteamOhosEnterNativeLoader
.hidden SteamOhosEnterNativeLoader
#if defined(__aarch64__)
.type SteamOhosEnterNativeLoader,%function
SteamOhosEnterNativeLoader:
    mov x16, x0
    mov sp, x1
    mov x0, xzr
    mov x29, xzr
    mov x30, xzr
    br x16
#elif defined(__x86_64__)
.type SteamOhosEnterNativeLoader,@function
SteamOhosEnterNativeLoader:
    mov %rdi, %rax
    mov %rsi, %rsp
    xor %ebp, %ebp
    xor %edx, %edx
    jmp *%rax
#else
#error Unsupported native ISA
#endif
.size SteamOhosEnterNativeLoader, .-SteamOhosEnterNativeLoader

// Copied verbatim into a dedicated RX page. No relocations, TLS, libc calls,
// old stack access or return path are permitted after this entry point.
.p2align 4
.global SteamOhosExecCleanupStart
.global SteamOhosExecCleanupEnd
.hidden SteamOhosExecCleanupStart
.hidden SteamOhosExecCleanupEnd
SteamOhosExecCleanupStart:
#if defined(__aarch64__)
    mov x19, x0
    mov sp, x1
    mov x21, x2
    mov x22, x3
1:
    cbz x22, 2f
    ldp x0, x1, [x21], #16
    mov x8, #215 // munmap
    svc #0
    cbnz x0, 3f
    sub x22, x22, #1
    b 1b
2:
    mov x0, xzr
    mov x29, xzr
    mov x30, xzr
    br x19
3:
    mov x0, #126
    mov x8, #94 // exit_group: never enter a partially reclaimed image
    svc #0
    b 3b
#elif defined(__x86_64__)
    mov %rdi, %r12
    mov %rsi, %rsp
    mov %rdx, %r14
    mov %rcx, %r15
1:
    test %r15, %r15
    jz 2f
    mov (%r14), %rdi
    mov 8(%r14), %rsi
    mov $11, %eax // munmap
    syscall
    test %rax, %rax
    jnz 3f
    add $16, %r14
    dec %r15
    jmp 1b
2:
    xor %ebp, %ebp
    xor %edx, %edx
    jmp *%r12
3:
    mov $126, %edi
    mov $231, %eax // exit_group
    syscall
    jmp 3b
#endif
#if defined(__aarch64__)
.p2align 4
.global SteamOhosExecFaultStart
.hidden SteamOhosExecFaultStart
SteamOhosExecFaultStart:
    sub sp, sp, #128
    mov x20, x1
    mov x21, x2
    adr x9, .Lexec_fault_text
    mov x10, sp
    mov x11, #(.Lexec_fault_end - .Lexec_fault_text)
.Lexec_fault_copy:
    ldrb w12, [x9], #1
    strb w12, [x10], #1
    subs x11, x11, #1
    b.ne .Lexec_fault_copy
    ldr x0, [x21, #440] // ucontext_t.uc_mcontext.pc, asserted in C++
    add x1, sp, #(.Lexec_fault_pc - .Lexec_fault_text)
    bl .Lexec_fault_hex
    ldr x0, [x21, #424] // uc_mcontext.regs[30]
    add x1, sp, #(.Lexec_fault_lr - .Lexec_fault_text)
    bl .Lexec_fault_hex
    ldr x0, [x20, #16] // siginfo_t.si_addr
    add x1, sp, #(.Lexec_fault_address - .Lexec_fault_text)
    bl .Lexec_fault_hex
    mov x0, #2
    mov x1, sp
    mov x2, #(.Lexec_fault_end - .Lexec_fault_text)
    mov x8, #64
    svc #0
.Lexec_fault_exit:
    mov x0, #139
    mov x8, #94
    svc #0
    b .Lexec_fault_exit
.Lexec_fault_hex:
    mov x2, #16
1:
    lsr x3, x0, #60
    cmp x3, #10
    add x4, x3, #48
    add x5, x3, #87
    csel x3, x4, x5, lo
    strb w3, [x1], #1
    lsl x0, x0, #4
    subs x2, x2, #1
    b.ne 1b
    ret
.Lexec_fault_text:
    .ascii "STEAMOHOS_EXEC_FAULT pc=0x"
.Lexec_fault_pc:
    .ascii "0000000000000000 lr=0x"
.Lexec_fault_lr:
    .ascii "0000000000000000 address=0x"
.Lexec_fault_address:
    .ascii "0000000000000000\n"
.Lexec_fault_end:
#endif
SteamOhosExecCleanupEnd:
.section .note.GNU-stack,"",%progbits
