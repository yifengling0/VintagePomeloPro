// Target ARM64 direct-kernel and PE32 actual-JIT microbench. Not a game FPS test.
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#if defined(__aarch64__)
#include "StrictMul24.h"
typedef extFloat80_t F80;
#else
typedef struct { uint64_t signif; uint16_t signExp; } __attribute__((packed)) F80;
extern void guest_mul_loop(const F80 *, const F80 *, F80 *, unsigned);
#endif
static volatile uint64_t sink;
static double elapsed(LARGE_INTEGER a,LARGE_INTEGER b,LARGE_INTEGER freq) {
 return (double)(b.QuadPart-a.QuadPart)*1e9/freq.QuadPart;
}
int main(int argc,char **argv) {
 FILE *out=argc>1?fopen(argv[1],"wb"):stdout;if(!out)return 2;
 LARGE_INTEGER freq,start,end;QueryPerformanceFrequency(&freq);
 F80 a={0},b={0},r={0};a.signif=UINT64_C(0x8123450000000000);a.signExp=0x3fff;
 b.signif=UINT64_C(0xf1abcd0000000000);b.signExp=0x3fff;
 const unsigned count=8000000;
#if defined(__aarch64__)
 for(unsigned pass=0;pass<5;pass++)for(unsigned mode=0;mode<2;mode++) {
  // Use a volatile array so neither the loop nor guards can be constant-folded.
  volatile F80 inputs[2]={a,b};
  QueryPerformanceCounter(&start);
  for(unsigned i=0;i<count;i++) {
   F80 x=inputs[i&1],y=inputs[(i+1)&1];
   struct softfloat_state state={0};state.roundingPrecision=32;
   if(mode && FEXTryStrictMul24(&state,x.signif,x.signExp,y.signif,y.signExp,&r.signif,&r.signExp)) {}
   else r=extF80_mul(&state,x,y);
   sink^=r.signif^r.signExp^state.exceptionFlags;
  }
  QueryPerformanceCounter(&end);
  fprintf(out,"pass=%u mode=%s count=%u ns_per_op=%.3f sink=%llu\n",pass,mode?"strict-mul24":"original-softfloat",count,elapsed(start,end,freq)/count,(unsigned long long)sink);
  fflush(out);
 }
#else
 uint16_t saved,cw=0x7f;__asm__ volatile("fnstcw %0;fldcw %1":"=m"(saved):"m"(cw));
 for(unsigned pass=0;pass<5;pass++) {
  QueryPerformanceCounter(&start);guest_mul_loop(&a,&b,&r,count);QueryPerformanceCounter(&end);
  sink^=r.signif^r.signExp;
  fprintf(out,"pass=%u mode=guest-jit count=%u ns_per_op=%.3f result=%016llx:%04x\n",pass,count,elapsed(start,end,freq)/count,(unsigned long long)r.signif,r.signExp);fflush(out);
 }
 __asm__ volatile("fldcw %0"::"m"(saved));
#endif
 fprintf(out,"complete=1\n");if(out!=stdout)fclose(out);return 0;
}
