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
.section .note.GNU-stack,"",%progbits
