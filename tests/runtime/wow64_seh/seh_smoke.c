/* PE32 regression for Box WoW64 fault -> x86 VEH -> resume.
 * Does not access game data. Build with i686-w64-mingw32-clang. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
extern void read_probe(void), write_probe(void);
extern char read_fault, read_resume, write_fault, write_resume;
__asm__(".text\n"
".globl _read_probe, _read_fault, _read_resume\n"
"_read_probe:\n xor %eax,%eax\n mov $0x31415926,%ecx\n movd %ecx,%xmm0\n fld1\n fldpi\n cmp $0x31415926,%ecx\n"
"_read_fault:\n mov 0x24(%eax),%eax\n_read_resume:\n pushf\n pop %eax\n fstp %st(0)\n fstp %st(0)\n ret\n"
".globl _write_probe, _write_fault, _write_resume\n"
"_write_probe:\n xor %eax,%eax\n mov $0x31415926,%ecx\n movd %ecx,%xmm0\n fld1\n fldpi\n cmp $0x31415926,%ecx\n"
"_write_fault:\n mov %ecx,0x28(%eax)\n_write_resume:\n pushf\n pop %eax\n fstp %st(0)\n fstp %st(0)\n ret\n");
static unsigned seen, errors;
static struct { DWORD ip,expected_ip,cx,flags,xmm,nparams,access,addr,sw; long double pi,one; } observations[2];
static LONG CALLBACK handler(EXCEPTION_POINTERS* p) {
    EXCEPTION_RECORD* r=p->ExceptionRecord;
    CONTEXT* c=p->ContextRecord;
    unsigned write = r->ExceptionAddress == &write_fault;
    if(r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
       (r->ExceptionAddress != &read_fault && !write)) return EXCEPTION_CONTINUE_SEARCH;
    long double pi=0, one=0;
    memcpy(&pi,c->FloatSave.RegisterArea,10); memcpy(&one,c->FloatSave.RegisterArea+10,10);
    uint32_t xmm0; memcpy(&xmm0, c->ExtendedRegisters + 160, 4);
    if(c->Eip != (DWORD)(uintptr_t)r->ExceptionAddress || c->Ecx != 0x31415926 ||
       ((c->FloatSave.StatusWord>>11)&7) != 6 || one != 1.0L || pi < 3.14159265358L || pi > 3.14159265360L ||
       !(c->EFlags & 0x40) || xmm0 != 0x31415926 || r->NumberParameters != 2 ||
       r->ExceptionInformation[0] != write || r->ExceptionInformation[1] != (write?0x28:0x24)) ++errors;
    if(seen<2) {
        observations[seen].ip=c->Eip; observations[seen].expected_ip=(DWORD)(uintptr_t)r->ExceptionAddress;
        observations[seen].sw=c->FloatSave.StatusWord;observations[seen].pi=pi;observations[seen].one=one;
        observations[seen].cx=c->Ecx; observations[seen].flags=c->EFlags;
        observations[seen].xmm=xmm0; observations[seen].nparams=r->NumberParameters;
        observations[seen].access=r->ExceptionInformation[0]; observations[seen].addr=r->ExceptionInformation[1];
    }
    ++seen;
    c->Eip=(DWORD)(uintptr_t)(write?&write_resume:&read_resume);
    return EXCEPTION_CONTINUE_EXECUTION;
}
int main(void) {
    BOOL sse2=IsProcessorFeaturePresent(PF_XMMI64_INSTRUCTIONS_AVAILABLE);
    if (!sse2) ++errors;
    unsigned char* code=VirtualAlloc(NULL,4096,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE);
    if (!code) return 3;
    memcpy(code, "\xb8\x2a\x00\x00\x00\xc3", 6);
    FlushInstructionCache(GetCurrentProcess(),code,6);
    int first=((int (*)(void))code)();
    MEMORY_BASIC_INFORMATION mbi;
    VirtualQuery(code,&mbi,sizeof(mbi));
    DWORD observed_protect=mbi.Protect;
    /* SMC tracking is host-internal; it must not alter Windows permissions. */
    if(first!=42 || mbi.Protect!=PAGE_EXECUTE_READWRITE) ++errors;
    code[1]=84;
    FlushInstructionCache(GetCurrentProcess(),code,6);
    int second=((int (*)(void))code)();
    if(second!=84) ++errors;
    VirtualFree(code,0,MEM_RELEASE);
    PVOID h=AddVectoredExceptionHandler(1,handler);
    if(!h) return 2;
    read_probe(); write_probe();
    RemoveVectoredExceptionHandler(h);
    FILE* f=fopen("C:\\windows\\temp\\box-seh-smoke.txt","w");
    if(f) { fprintf(f,"seen=%u errors=%u result=%s\n",seen,errors,seen==2&&!errors?"PASS":"FAIL");
        for(unsigned i=0;i<seen&&i<2;i++) fprintf(f,"case=%u ip=%08lx expected=%08lx cx=%08lx flags=%08lx xmm=%08lx params=%lu access=%lu addr=%08lx sw=%04lx pi=%.12f one=%.12f\n",i,observations[i].ip,observations[i].expected_ip,observations[i].cx,observations[i].flags,observations[i].xmm,observations[i].nparams,observations[i].access,observations[i].addr,observations[i].sw,(double)observations[i].pi,(double)observations[i].one);
        fprintf(f,"sse2=%d\n",sse2);
        fprintf(f,"smc_first=%d smc_second=%d guest_protect=%08lx expected=%08lx\n",first,second,observed_protect,(DWORD)PAGE_EXECUTE_READWRITE);
        fclose(f); }
    return seen==2&&!errors?0:1;
}
