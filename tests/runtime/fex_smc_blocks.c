/* Synthetic code only: no Mono, Unity, or game assets are needed.
 * Build with i686-w64-mingw32-clang or x86_64-w64-mingw32-clang -O2.
 * Run in a fresh FEX process, with x87 reduced precision both 0 and 1.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef void (__cdecl *probe_fn)(uint32_t *);
static const unsigned char body[] = {
#ifdef _WIN64
    0x48,0x89,0xc8,           /* mov rax,rcx (Win64 first argument) */
#else
    0x8b,0x44,0x24,0x04,     /* mov eax,[esp+4] (cdecl argument) */
#endif
    0xd9,0xe8,0xd9,0x18,      /* fld1; fstp [rax] */
    0xd9,0xee,0xd9,0x58,0x04, /* fldz; fstp [rax+4] */
    0xd9,0xe8,0xd9,0x58,0x08,
    0xd9,0xee,0xd9,0x58,0x0c,
    0xd9,0xe8,0xd9,0x58,0x10,
    0xd9,0xee,0xd9,0x58,0x14,
    0xd9,0xe8,0xd9,0x58,0x18,
    0xd9,0xee,0xd9,0x58,0x1c,
    0xf3,0x0f,0x6f,0x40,0x40, /* movdqu xmm0,[rax+64] */
    0x66,0x0f,0xd7,0xc8,      /* pmovmskb ecx,xmm0 */
    0x89,0x48,0x20,
    0x66,0x0f,0x74,0xc0,      /* pcmpeqb xmm0,xmm0 */
    0x66,0x0f,0xd7,0xc8,
    0x89,0x48,0x24,
    0x66,0x0f,0xef,0xc0,      /* pxor xmm0,xmm0 */
    0x66,0x0f,0xd7,0xc8,
    0x89,0x48,0x28,
    0xb9,0xff,0xff,0xff,0xff, /* mov ecx,-1 */
    0x83,0xc1,0x01,           /* add ecx,1: CF=1 */
    0xba,0x00,0x00,0x00,0x00, /* mov edx,0: preserve CF */
    0x83,0xd2,0x00,           /* adc edx,0 */
    0x89,0x50,0x2c,
    0x83,0xfa,0x01,           /* cmp edx,1 */
    0x0f,0x94,0xc2,           /* sete dl */
    0x0f,0xb6,0xd2,
    0x89,0x50,0x30,0xc3
};

int main(int argc, char **argv) {
    FILE *out = argc > 1 ? fopen(argv[1], "w") : stdout;
    if (!out) return 2;
    int cases = 0, errors = 0;
    unsigned short saved_cw;
    __asm__ volatile("fnstcw %0" : "=m"(saved_cw));
    for (int writable = 0; writable < 2; ++writable) {
        unsigned char *code = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        DWORD old;
        if (!code) return 3;
        memcpy(code, body, sizeof(body));
        if (!VirtualProtect(code, 4096, writable ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ, &old)) return 4;
        if (!FlushInstructionCache(GetCurrentProcess(), code, sizeof(body))) return 5;
        for (int changed = 0; changed < 2; ++changed) {
            if (changed) {
                if (!VirtualProtect(code, 4096, PAGE_EXECUTE_READWRITE, &old)) return 6;
                /* Real code modification: first FLD1 becomes FLDZ. */
#ifdef _WIN64
                code[4] = 0xee;
#else
                code[5] = 0xee;
#endif
                if (!VirtualProtect(code, 4096, writable ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ, &old)) return 7;
                if (!FlushInstructionCache(GetCurrentProcess(), code, sizeof(body))) return 8;
            }
            for (int precision = 0; precision < 3; ++precision) for (int top = 0; top < 8; ++top) {
                uint32_t values[20] = {0};
                unsigned char *seed = (unsigned char *)(values + 16);
                for (int i = 0; i < 16; ++i) seed[i] = i % 2 ? 0 : 0x80;
                unsigned short cw = precision == 0 ? 0x037f : precision == 1 ? 0x027f : 0x007f;
                __asm__ volatile("fninit; fldcw %0" :: "m"(cw));
                for (int i = 0; i < top; ++i) __asm__ volatile("fincstp");
                ((probe_fn)code)(values);
                __asm__ volatile("fninit; fldcw %0" :: "m"(saved_cw));
                for (int i = 0; i < 13; ++i) {
                    uint32_t expected = i < 8 ? (i % 2 || (i == 0 && changed) ? 0 : 0x3f800000) :
                        i == 8 ? 0x5555 : i == 9 ? 0xffff : i == 10 ? 0 : 1;
                    ++cases;
                    if (values[i] != expected) {
                        ++errors;
                        if (errors < 24) fprintf(out, "RWX=%d modified=%d CW=%04x TOP=%d field=%d got=%08x expected=%08x\n",
                            writable, changed, cw, top, i, values[i], expected);
                    }
                }
            }
        }
        VirtualFree(code, 0, MEM_RELEASE);
    }
    fprintf(out, "arch=%s cases=%d errors=%d result=%s\n",
#ifdef _WIN64
        "x64",
#else
        "x86",
#endif
        cases, errors, errors ? "FAIL" : "PASS");
    if (out != stdout) fclose(out);
    return errors ? 1 : 0;
}
