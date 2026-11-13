#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

typedef struct __attribute__((packed)) { uint64_t sig; uint16_t se; } ext80;
static uint64_t rng=0x81358745abcdef01ULL;
static uint64_t random64(void) { rng^=rng<<13; rng^=rng>>7; rng^=rng<<17; return rng; }
static uint64_t mix(uint64_t h,uint64_t x) { return (h ^ x)*1099511628211ULL; }
#ifdef __i386__
#define SAVE_FLAGS "pushfl; popl %k[flags]; "
#else
#define SAVE_FLAGS "pushfq; popq %[flags]; "
#endif
static unsigned check(ext80 v, uint16_t cw, uint64_t *bits, uint64_t *status)
{
    uint32_t f32; uint64_t f64; uint16_t s32,s64; uintptr_t flags;
    __asm__ volatile("fnclex; fldcw %[cw]; fldt %[v]; movl $0, %%eax; cmpl $1, %%eax; fstps %[out]; "
        SAVE_FLAGS "fnstsw %[sw]"
        : [out] "=m"(f32), [sw] "=m"(s32), [flags] "=r"(flags)
        : [cw] "m"(cw), [v] "m"(v) : "eax","cc","st","memory");
    unsigned bad=(flags & 0x8d5)!=0x95;
    __asm__ volatile("fnclex; fldcw %[cw]; fldt %[v]; movl $0, %%eax; cmpl $1, %%eax; fstpl %[out]; "
        SAVE_FLAGS "fnstsw %[sw]"
        : [out] "=m"(f64), [sw] "=m"(s64), [flags] "=r"(flags)
        : [cw] "m"(cw), [v] "m"(v) : "eax","cc","st","memory");
    bad+=(flags & 0x8d5)!=0x95;
    *bits=mix(mix(*bits,f32),f64);
    *status=mix(mix(*status,s32&1),s64&1);
    return bad;
}
int main(void)
{
    FILE *out=stdout;
#ifdef _WIN32
    char path[MAX_PATH];
    snprintf(path,sizeof(path),"C:\\vp-exact-store-%lu.tsv",(unsigned long)GetCurrentProcessId());
    out=fopen(path,"wb"); if(!out) return 2;
#endif
    uint16_t saved; __asm__ volatile("fnstcw %0":"=m"(saved));
    unsigned all_bad=0;
    fprintf(out,"category\tpc\trc\tcases\tbits\tinvalid\tflags_fail\n");
    for(unsigned pc=0;pc<4;++pc) if(pc!=1) for(unsigned rc=0;rc<4;++rc) {
        uint16_t cw=(uint16_t)(0x7f | pc<<8 | rc<<10);
        for(unsigned category=0;category<4;++category) {
            uint64_t bits=1469598103934665603ULL,status=bits;
            unsigned count=0,bad=0; rng=0x81358745abcdef01ULL;
            if(category<2) {
                unsigned max_exp=category==0?254:2046;
                unsigned shift=category==0?40:11;
                unsigned bias=category==0?0x3f80:0x3c00;
                for(unsigned sign=0;sign<2;++sign) for(unsigned e=1;e<=max_exp;++e) for(unsigned j=0;j<4;++j) {
                    uint64_t sig=(random64()>>shift)<<shift; sig|=1ULL<<63;
                    if(j==0)sig=1ULL<<63;
                    if(j==1)sig=~((1ULL<<shift)-1);
                    ext80 v={sig,(uint16_t)(bias+e+(sign<<15))};
                    bad+=check(v,cw,&bits,&status);++count;
                }
            } else if(category==2) {
                const uint16_t exps[]={0,1,0x3f80,0x3f81,0x407e,0x407f,0x3c00,0x3c01,0x43fe,0x43ff,0x7ffe,0x7fff};
                const uint64_t sigs[]={0,1,0x7fffffffffffffffULL,0x8000000000000000ULL,0x8000000000000001ULL,
                    0x80000000000007ffULL,0x8000000000000800ULL,0x800000ffffffffffULL,0xc000000000000000ULL,~0ULL};
                for(unsigned sign=0;sign<2;++sign) for(unsigned e=0;e<sizeof(exps)/sizeof(*exps);++e)
                    for(unsigned s=0;s<sizeof(sigs)/sizeof(*sigs);++s) {
                        ext80 v={sigs[s],(uint16_t)(exps[e]+(sign<<15))};
                        bad+=check(v,cw,&bits,&status);++count;
                    }
            } else for(unsigned i=0;i<12000;++i) {
                ext80 v={random64(),(uint16_t)random64()};
                bad+=check(v,cw,&bits,&status);++count;
            }
            fprintf(out,"%u\t%u\t%u\t%u\t%016llx\t%016llx\t%u\n",category,pc,rc,count,
                (unsigned long long)bits,(unsigned long long)status,bad);
            fflush(out); all_bad+=bad;
        }
    }
    __asm__ volatile("fnclex; fldcw %0"::"m"(saved));
    fprintf(out,"complete=1 flags_fail=%u\n",all_bad);
#ifdef _WIN32
    LARGE_INTEGER freq,start,end;
    QueryPerformanceFrequency(&freq);
    ext80 input={0x9234560000000000ULL,0x3fff};
    volatile uint32_t result32=0;
    volatile uint64_t result64=0;
    for(unsigned run=0;run<3;++run) {
        QueryPerformanceCounter(&start);
        for(unsigned i=0;i<2000000;++i)
            __asm__ volatile("fldt %1; fstps %0":"=m"(result32):"m"(input):"st","memory");
        QueryPerformanceCounter(&end);
        fprintf(out,"bench32\t%u\t%.4f\t%08x\n",run,
            (double)(end.QuadPart-start.QuadPart)*1000/freq.QuadPart,(unsigned)result32);
        QueryPerformanceCounter(&start);
        for(unsigned i=0;i<2000000;++i)
            __asm__ volatile("fldt %1; fstpl %0":"=m"(result64):"m"(input):"st","memory");
        QueryPerformanceCounter(&end);
        fprintf(out,"bench64\t%u\t%.4f\t%016llx\n",run,
            (double)(end.QuadPart-start.QuadPart)*1000/freq.QuadPart,(unsigned long long)result64);
    }
    fprintf(out,"bench_complete=1\n");
#endif
    if(out!=stdout)fclose(out);
    return all_bad?1:0;
}
