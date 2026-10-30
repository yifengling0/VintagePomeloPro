// SPDX-License-Identifier: LGPL-2.1-or-later
// GNU port of VPP SIMD-zero/POP fault probe ideas. Reference identities are
// retained in docs/migration.json. No Wine, game or graphics dependency.
#define _GNU_SOURCE 1
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

#if !defined(__i386__) && !defined(__x86_64__)
#error x86 instruction correctness probe
#endif
static const float scalar=800.0f;
#define SIMD_CASE(name,op) \
__attribute__((noinline)) static void name(uint32_t* out) { \
    __asm__ volatile(op " %%xmm1,%%xmm1\n\t" \
      "movups %%xmm1,0(%0)\n\t" "movss %1,%%xmm1\n\t" \
      op " %%xmm3,%%xmm3\n\t" op " %%xmm0,%%xmm0\n\t" \
      "movups %%xmm0,16(%0)\n\t" "movups %%xmm1,32(%0)\n\t" \
      "movups %%xmm3,48(%0)\n\t" : : "r"(out),"m"(scalar) \
      : "xmm0","xmm1","xmm3","memory"); }
SIMD_CASE(xorps_case,"xorps")
SIMD_CASE(xorpd_case,"xorpd")
SIMD_CASE(pxor_case,"pxor")
static void* fault_page;
static size_t page_size;
static volatile sig_atomic_t faults;
static uintptr_t fault_sp;
static uintptr_t expected_fault_sp;
static void handler(int number,siginfo_t* info,void* opaque) {
    if(number!=SIGSEGV||info->si_addr!=fault_page||faults++)_exit(91);
    ucontext_t* context=opaque;
#ifdef __i386__
    fault_sp=(uintptr_t)context->uc_mcontext.gregs[REG_ESP];
    expected_fault_sp=(uintptr_t)context->uc_mcontext.gregs[REG_EDX]-sizeof(uintptr_t);
#else
    fault_sp=(uintptr_t)context->uc_mcontext.gregs[REG_RSP];
    expected_fault_sp=(uintptr_t)context->uc_mcontext.gregs[REG_RDX]-sizeof(uintptr_t);
#endif
    if(mprotect(fault_page,page_size,PROT_READ|PROT_WRITE))_exit(92);
}
int main(void) {
    alarm(30);setvbuf(stdout,NULL,_IONBF,0);unsigned failures=0;
    void (*cases[])(uint32_t*)={xorps_case,xorpd_case,pxor_case};
    const char* names[]={"xorps","xorpd","pxor"};
    for(unsigned i=0;i<3;++i) {
        unsigned bad=0;
        for(unsigned repeat=0;repeat<64;++repeat) {
            uint32_t output[16];memset(output,0xcd,sizeof(output));cases[i](output);
            for(unsigned lane=0;lane<16;++lane)
                if(output[lane]!=(lane==8?UINT32_C(0x44480000):0))++bad;
        }
        printf("HSL_FEX_CPU case=%s repeats=64 bad=%u\n",names[i],bad);failures+=bad!=0;
    }
    page_size=(size_t)sysconf(_SC_PAGESIZE);
    // A guest-visible destination fault must expose pre-POP SP, then retry
    // once after the handler repairs permissions. The function restores its
    // saved SP so a double-pop is reported rather than corrupting its return.
    struct sigaction action={0},old;action.sa_sigaction=handler;action.sa_flags=SA_SIGINFO;
    sigemptyset(&action.sa_mask);if(sigaction(SIGSEGV,&action,&old))return 3;
    for(unsigned smc=0;smc<2;++smc) {
    unsigned char* code=mmap(NULL,page_size,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(code==MAP_FAILED)return 2;
    fault_page=smc?(void*)(code+128):mmap(NULL,page_size,PROT_READ,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(fault_page==MAP_FAILED)return 2;
    faults=0;fault_sp=0;expected_fault_sp=0;
    unsigned char* p=code;
#ifdef __x86_64__
    *p++=0x48;
#endif
    *p++=0x89;*p++=0xe2; // mov saved SP to DX
#ifdef __x86_64__
    *p++=0x48;
#endif
    *p++=0xb9;uintptr_t destination=(uintptr_t)fault_page;
    memcpy(p,&destination,sizeof(destination));p+=sizeof(destination); // mov CX,dest
    *p++=0x68;uint32_t pattern=0x12345678;memcpy(p,&pattern,4);p+=4;
    *p++=0x8f;*p++=0x01; // pop [CX]
#ifdef __x86_64__
    *p++=0x48;
#endif
    *p++=0x89;*p++=0xe0; // mov AX,SP
#ifdef __x86_64__
    *p++=0x48;
#endif
    *p++=0x29;*p++=0xd0; // sub AX,saved SP
#ifdef __x86_64__
    *p++=0x48;
#endif
    *p++=0x89;*p++=0xd4;*p++=0xc3; // restore SP, ret
    __builtin___clear_cache((char*)code,(char*)p);
    intptr_t delta=((intptr_t(*)(void))code)();uintptr_t stored=0;
    memcpy(&stored,fault_page,sizeof(stored));
    int precise=smc?faults==0:faults==1&&fault_sp&&fault_sp==expected_fault_sp;
    int ok=!delta&&stored==pattern&&precise;
    printf("HSL_FEX_CPU case=%s delta=%ld stored=%lx faults=%d precise=%d pass=%d\n",
        smc?"pop-smc-write":"pop-destination-fault",(long)delta,(unsigned long)stored,(int)faults,precise,ok);failures+=!ok;
    if(!smc)munmap(fault_page,page_size);
    munmap(code,page_size);
    }
    sigaction(SIGSEGV,&old,NULL);
    // POP [SP] addresses the destination using the incremented SP. Verify
    // that delaying architectural SP commitment does not change this rule.
    static const unsigned char stack_case[]={
#ifdef __x86_64__
        0x48,
#endif
        0x89,0xe2,0x68,0,0,0,0,0x68,0x78,0x56,0x34,0x12,
        0x8f,0x04,0x24,0x58,
#ifdef __x86_64__
        0x48,
#endif
        0x89,0xd4,0xc3
    };
    void* stack_code=mmap(NULL,page_size,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(stack_code==MAP_FAILED)return 4;
    memcpy(stack_code,stack_case,sizeof(stack_case));
    __builtin___clear_cache(stack_code,(char*)stack_code+sizeof(stack_case));
    uintptr_t stack_result=((uintptr_t(*)(void))stack_code)();
    printf("HSL_FEX_CPU case=pop-stack-address stored=%lx pass=%d\n",(unsigned long)stack_result,stack_result==0x12345678);
    failures+=stack_result!=0x12345678;munmap(stack_code,page_size);
    printf("HSL_FEX_CPU_RESULT bits=%zu failures=%u\n",8*sizeof(void*),failures);
    return failures?1:0;
}
