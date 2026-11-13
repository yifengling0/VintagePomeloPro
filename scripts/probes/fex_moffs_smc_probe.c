/* A2/A3 stores must reconstruct their own RIP, never replay preceding SP
 * arithmetic. Exercise SMC write protection with aligned/unaligned targets. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static void ptr_prefix(unsigned char **p) {
#ifdef _WIN64
    *(*p)++ = 0x48;
#else
    (void)p;
#endif
}
int main(int argc, char **argv) {
    FILE *out = fopen(argc > 1 ? argv[1] : "C:\\vp-moffs-smc.tsv", "w");
    unsigned checks = 0, failures = 0;
    if (!out) return 2;
    setvbuf(out, NULL, _IONBF, 0);
    fprintf(out, "pointer_bits\t%u\n", (unsigned)(sizeof(void *) * 8));
    for (int step = 0; step < 3; ++step)
    for (int unaligned = 0; unaligned < 2; ++unaligned)
    for (int size = 1; size <= 3; ++size) {
        unsigned char *code = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        unsigned char *p = code;
        const uintptr_t pattern = 0x12345678 + step;
        uintptr_t address = (uintptr_t)(code + 256 + unaligned);
        if (!code) return 3;
        /* Reserve owned stack space, save SP in dx, push three words. */
        ptr_prefix(&p); *p++=0x83; *p++=0xec; *p++=0x40;
        ptr_prefix(&p); *p++=0x89; *p++=0xe2;
        for (int n=0;n<3;++n) { *p++=0x6a; *p++=0x01; }
        /* The offset MOV must not replay this preceding adjustment. */
        ptr_prefix(&p); *p++=0x83; *p++=0xc4; *p++=3*sizeof(void*);
        ptr_prefix(&p); *p++=0xb8; memcpy(p,&pattern,sizeof(pattern)); p+=sizeof(pattern);
        if (size==2) *p++=0x66;
        if (size==3) ptr_prefix(&p);
        *p++=size==1 ? 0xa2 : 0xa3;
        memcpy(p,&address,sizeof(address)); p+=sizeof(address);
        ptr_prefix(&p); *p++=0x89; *p++=0xe0;
        ptr_prefix(&p); *p++=0x29; *p++=0xd0;
        ptr_prefix(&p); *p++=0x89; *p++=0xd4;
        ptr_prefix(&p); *p++=0x83; *p++=0xc4; *p++=0x40; *p++=0xc3;
        FlushInstructionCache(GetCurrentProcess(),code,4096);
        fprintf(out,"begin\t%u\n",checks);
        intptr_t delta=((intptr_t(*)(void))code)();
        uintptr_t value=0;
        unsigned width=size==3 ? sizeof(void*) : size;
        memcpy(&value,(void*)address,width);
        uintptr_t expected=width==1 ? pattern&0xff : width==2 ? pattern&0xffff : pattern;
        int pass=!delta && value==expected;
        fprintf(out,"moffs\t%u\twidth=%u\tunaligned=%d\tsp_delta=%lld\tstored=%llx\texpected=%llx\tpass=%d\n",
                checks++,width,unaligned,(long long)delta,(unsigned long long)value,(unsigned long long)expected,pass);
        failures+=!pass;
        VirtualFree(code,0,MEM_RELEASE);
    }
    fprintf(out,"summary\tchecks=%u\tfailures=%u\n",checks,failures);
    fclose(out);
    return failures ? 1 : 0;
}
