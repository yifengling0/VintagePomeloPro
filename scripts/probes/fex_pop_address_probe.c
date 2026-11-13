/* Generated x86 POP tests: stack-based post-increment addresses, partial
 * operands, SIB, register destinations, and executable-page write replay.
 * No global FEX single-step/SMC setting is used. All clobbers are volatile. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void bytes(unsigned char **p, const unsigned char *v, size_t n)
{ memcpy(*p, v, n); *p += n; }
#define EMIT(...) do { const unsigned char v[] = {__VA_ARGS__}; bytes(&p, v, sizeof(v)); } while (0)
static void ptr_imm(unsigned char **p, unsigned opcode, uintptr_t v)
{
#ifdef _WIN64
    *(*p)++ = 0x48;
#endif
    *(*p)++ = (unsigned char)opcode;
    bytes(p, (unsigned char *)&v, sizeof(v));
}
static void width_prefix(unsigned char **p)
{
#ifdef _WIN64
    *(*p)++ = 0x48;
#else
    (void)p;
#endif
}
static void push_pattern(unsigned char **p, int word, uint32_t value)
{
    if (word) *(*p)++ = 0x66;
    *(*p)++ = 0x68;
    bytes(p, (unsigned char *)&value, word ? 2 : 4);
}
static void finish(unsigned char **p)
{
    /* delta = SP - saved SP; restore SP before returning. */
    width_prefix(p); *(*p)++ = 0x89; *(*p)++ = 0xe0;
    width_prefix(p); *(*p)++ = 0x29; *(*p)++ = 0xd0;
    width_prefix(p); *(*p)++ = 0x89; *(*p)++ = 0xd4;
}

static unsigned check(FILE *out, unsigned id, const char *kind, int word,
                      intptr_t delta, uintptr_t observed, uintptr_t expected)
{
    int pass = !delta && observed == expected;
    fprintf(out, "case\t%u\t%s\tword=%d\tsp_delta=%lld\tstored=%llx\texpected=%llx\tpass=%d\n",
            id, kind, word, (long long)delta, (unsigned long long)observed,
            (unsigned long long)expected, pass);
    return !pass;
}

int main(int argc, char **argv)
{
    FILE *out = fopen(argc > 1 ? argv[1] : "C:\\vp-pop-address.tsv", "w");
    unsigned id = 0, failures = 0;
    const uint32_t pattern = 0x12345678;
    const uintptr_t sentinel = (uintptr_t)0x6a5b4c3d2e1f9081ULL;
    if (!out) return 2;
    setvbuf(out, NULL, _IONBF, 0);
    fprintf(out, "pointer_bits\t%u\n", (unsigned)(sizeof(void *) * 8));

    for (int smc = 0; smc != 2; ++smc)
    for (int word = 0; word != 2; ++word)
    for (int mode = 0; mode != 4; ++mode) {
        unsigned char *code = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        unsigned char *data = smc ? code : VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        unsigned char *p = code;
        uintptr_t *target = (uintptr_t *)(data + 256);
        if (!code || !data) return 3;
        *target = sentinel;
        width_prefix(&p); EMIT(0x89, 0xe2); /* save SP in dx */
        if (mode) ptr_imm(&p, 0xb9, (uintptr_t)target - (mode == 1 ? 0 : mode == 2 ? 8 : 16));
        if (mode == 3) EMIT(0xb8, 0x08, 0, 0, 0); /* index = 8 */
        push_pattern(&p, word, pattern);
        if (word) EMIT(0x66);
        if (!mode) {
            EMIT(0x8f, 0x05);
#ifdef _WIN64
            /* RIP-relative encoding cannot reach a separate arbitrary page:
             * VirtualAlloc pages are close here; reject rather than truncate. */
            intptr_t rel = (unsigned char *)target - (p + 4);
            if (rel < INT32_MIN || rel > INT32_MAX) return 4;
            int32_t displacement = (int32_t)rel;
#else
            uint32_t displacement = (uint32_t)(uintptr_t)target;
#endif
            bytes(&p, (unsigned char *)&displacement, 4);
        } else if (mode == 1) EMIT(0x8f, 0x01);       /* [cx] */
        else if (mode == 2) EMIT(0x8f, 0x41, 0x08);  /* [cx+8] */
        else EMIT(0x8f, 0x44, 0x01, 0x08);          /* [cx+ax+8] */
        finish(&p); EMIT(0xc3);
        FlushInstructionCache(GetCurrentProcess(), code, 4096);
        fprintf(out, "begin\t%u\n", id);
        intptr_t delta = ((intptr_t (*)(void))code)();
        uintptr_t expected = word ? (sentinel & ~(uintptr_t)0xffff) | (pattern & 0xffff) : pattern;
        char kind[64]; snprintf(kind, sizeof(kind), "%s-memory-mode%d", smc ? "smc" : "rw", mode);
        failures += check(out, id++, kind, word, delta, *target, expected);
        if (!smc) VirtualFree(data, 0, MEM_RELEASE);
        VirtualFree(code, 0, MEM_RELEASE);
    }

    for (int word = 0; word != 2; ++word)
    for (int index = 0; index != 2; ++index) {
        unsigned char *code = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        unsigned char *p = code;
        uintptr_t observed = 0;
        if (!code) return 3;
        width_prefix(&p); EMIT(0x83, 0xec, 0x40); /* reserve our own stack slots */
        width_prefix(&p); EMIT(0x89, 0xe2);
        ptr_imm(&p, 0xb9, (uintptr_t)&observed);
        ptr_imm(&p, 0xb8, sentinel);
        width_prefix(&p); EMIT(0x89, 0x44, 0x24, 0x10);
        if (index) EMIT(0xb8, 0x08, 0, 0, 0);
        push_pattern(&p, word, pattern);
        if (word) EMIT(0x66);
        if (index) EMIT(0x8f, 0x44, 0x04, 0x08); /* [post-SP+ax+8] */
        else EMIT(0x8f, 0x44, 0x24, 0x10);      /* [post-SP+16] */
        width_prefix(&p); EMIT(0x8b, 0x44, 0x24, 0x10);
        width_prefix(&p); EMIT(0x89, 0x01);
        finish(&p);
        width_prefix(&p); EMIT(0x83, 0xc4, 0x40, 0xc3);
        FlushInstructionCache(GetCurrentProcess(), code, 4096);
        fprintf(out, "begin\t%u\n", id);
        intptr_t delta = ((intptr_t (*)(void))code)();
        uintptr_t expected = word ? (sentinel & ~(uintptr_t)0xffff) | (pattern & 0xffff) : pattern;
        failures += check(out, id++, index ? "post-SP-index" : "post-SP-base", word, delta, observed, expected);
        VirtualFree(code, 0, MEM_RELEASE);
    }

    for (int word = 0; word != 2; ++word)
    for (int sp = 0; sp != 2; ++sp) {
        unsigned char *code = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        unsigned char *p = code;
        uintptr_t observed = 0;
        if (!code) return 3;
        width_prefix(&p); EMIT(0x89, 0xe2);
        ptr_imm(&p, 0xb9, (uintptr_t)&observed);
        ptr_imm(&p, 0xb8, sentinel);
        if (sp) { if (word) EMIT(0x66); EMIT(0x52); }
        else push_pattern(&p, word, pattern);
        if (word) EMIT(0x66);
        EMIT(sp ? 0x5c : 0x58);
        width_prefix(&p); EMIT(0x89, 0x01);
        finish(&p); EMIT(0xc3);
        FlushInstructionCache(GetCurrentProcess(), code, 4096);
        fprintf(out, "begin\t%u\n", id);
        intptr_t delta = ((intptr_t (*)(void))code)();
        uintptr_t expected = sp ? sentinel : word ? (sentinel & ~(uintptr_t)0xffff) | (pattern & 0xffff) : pattern;
        failures += check(out, id++, sp ? "register-SP" : "register-AX", word, delta, observed, expected);
        VirtualFree(code, 0, MEM_RELEASE);
    }
    fprintf(out, "summary\tchecks=%u\tfailures=%u\n", id, failures);
    fclose(out);
    return failures ? 1 : 0;
}
