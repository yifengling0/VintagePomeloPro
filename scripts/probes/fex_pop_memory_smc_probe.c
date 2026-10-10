/* POP to a writable executable page must not commit SP before a write fault.
 * The generated function saves/restores SP itself so a double-pop is reported
 * as data and SP corruption instead of jumping through a damaged return slot. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned char *emit(unsigned char *p, const unsigned char *bytes, size_t n)
{ memcpy(p, bytes, n); return p + n; }

int main(int argc, char **argv)
{
    FILE *out = fopen(argc > 1 ? argv[1] : "C:\\vp-pop-smc.tsv", "w");
    unsigned failures = 0;
    if (!out) return 2;
    setvbuf(out, NULL, _IONBF, 0);
    fprintf(out, "pointer_bits\t%u\n", (unsigned)(sizeof(void *) * 8));
    for (unsigned step = 0; step < 8; ++step) {
        unsigned char *code = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        unsigned char *p = code, *operand;
        uint32_t pattern = 0x12340000 + step;
        if (!code) return 3;
        /* mov edx,esp / mov rdx,rsp */
#ifdef _WIN64
        *p++ = 0x48;
#endif
        p = emit(p, (unsigned char[]){0x89, 0xe2, 0x68}, 3);
        memcpy(p, &pattern, 4); p += 4;
        /* pop dword ptr [absolute] / pop qword ptr [rip + displacement] */
        p = emit(p, (unsigned char[]){0x8f, 0x05}, 2);
        operand = p; p += 4;
#ifdef _WIN64
        int32_t address = (int32_t)((code + 128) - p);
#else
        uint32_t address = (uint32_t)(uintptr_t)(code + 128);
#endif
        memcpy(operand, &address, 4);
        /* eax = current SP - saved SP; restore SP before ret */
#ifdef _WIN64
        *p++ = 0x48;
#endif
        p = emit(p, (unsigned char[]){0x89, 0xe0}, 2);
#ifdef _WIN64
        *p++ = 0x48;
#endif
        p = emit(p, (unsigned char[]){0x29, 0xd0}, 2);
#ifdef _WIN64
        *p++ = 0x48;
#endif
        p = emit(p, (unsigned char[]){0x89, 0xd4, 0xc3}, 3);
        FlushInstructionCache(GetCurrentProcess(), code, 4096);
        fprintf(out, "begin\t%u\n", step);
        intptr_t delta = ((intptr_t (*)(void))code)();
        uintptr_t stored = 0;
        memcpy(&stored, code + 128, sizeof(stored));
        int pass = !delta && stored == pattern;
        fprintf(out, "pop\t%u\tsp_delta=%lld\tstored=%llx\texpected=%x\tpass=%d\n",
                step, (long long)delta, (unsigned long long)stored, pattern, pass);
        failures += !pass;
        VirtualFree(code, 0, MEM_RELEASE);
    }
    fprintf(out, "summary\tchecks=8\tfailures=%u\n", failures);
    fclose(out);
    return failures ? 1 : 0;
}
