// Diagnostic-only PE32 test. Executes real guest instructions and consumes
// every result. ABI padding and the stack-pointer snapshot are not compared.
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct { uint64_t sig; uint16_t exp; } __attribute__((packed)) F80;
typedef struct {
  F80 value; uint16_t sw,cw,pad; uint32_t mxcsr,flags,regs[8],xmm[32];
} Observation;
_Static_assert(sizeof(Observation)==184,"assembly layout");
uint32_t x87_patterns[32];
typedef void (*Probe)(const F80*,const F80*,uint32_t,Observation*);
extern void x87_add(const F80*,const F80*,uint32_t,Observation*);
extern void x87_sub(const F80*,const F80*,uint32_t,Observation*);
extern void x87_mul(const F80*,const F80*,uint32_t,Observation*);
extern void x87_div(const F80*,const F80*,uint32_t,Observation*);
static Probe probes[]={x87_add,x87_sub,x87_mul,x87_div};
static const F80 edge[]={
 {0,0},{0,0x8000},{0x8000000000000000ULL,0x3fff},
 {0x8000000000000000ULL,0xbfff},{0xffffffffffffffffULL,0x7ffe},
 {0x8000000000000000ULL,1},{1,0},
 {0x8000000000000000ULL,0x7fff},{0xc000000000000123ULL,0x7fff},
 {0x8000000000000123ULL,0x7fff},{0x4000000000000000ULL,0x3fff},
 {0x8000000000000001ULL,0x3fff}};
static uint64_t hash=0xcbf29ce484222325ULL;
static void consume(const void*ptr,size_t n) {
 const uint8_t*p=ptr;while(n--){hash^=*p++;hash*=0x100000001b3ULL;}
}
static uint32_t next(uint32_t*s){*s^=*s<<13;*s^=*s>>17;*s^=*s<<5;return *s;}
int main(int argc,char**argv){
 if(argc!=2)return 2;
 FILE*out=fopen(argv[1],"wb");if(!out)return 3;
 for(unsigned i=0;i<32;i++)x87_patterns[i]=0x9e3779b9U*(i+1);
 uint32_t state=0x6d2b79f5U,fail=0,count=0;
 uint16_t oldcw;uint32_t oldmx;
 __asm__ volatile("fnstcw %0;stmxcsr %1":"=m"(oldcw),"=m"(oldmx));
 const uint32_t expect[]={0x768696a6,0x65758595,0x54647484,0,
   0x43536373,0x32425262,0x21314151,0x10203040};
 for(unsigned pc=0;pc<3;pc++)for(unsigned rc=0;rc<4;rc++){
  uint32_t cw=0x7f|((pc==0?0:pc==1?2:3)<<8)|(rc<<10);
  for(unsigned i=0;i<4096;i++){
   F80 a,b;
   if(i<144){a=edge[i/12];b=edge[i%12];}
   else{
    a.sig=((uint64_t)next(&state)<<32)|next(&state)|0x8000000000000000ULL;
    b.sig=((uint64_t)next(&state)<<32)|next(&state)|0x8000000000000000ULL;
    if(i&1){a.sig&=0xffffff0000000000ULL;b.sig&=0xffffff0000000000ULL;}
    a.exp=(uint16_t)(0x3fc0+(next(&state)&127));
    b.exp=(uint16_t)(0x3fc0+(next(&state)&127));
    a.exp|=(next(&state)&1)<<15;b.exp|=(next(&state)&1)<<15;
   }
   for(unsigned op=0;op<4;op++){
    Observation o={0};probes[op](&a,&b,cw,&o);count++;
    if(o.cw!=cw || (o.flags&0x8d5)!=0x8d5 ||
        memcmp(o.xmm,x87_patterns,sizeof(o.xmm)))fail++;
    for(unsigned g=0;g<8;g++)if(g!=3&&o.regs[g]!=expect[g])fail++;
    consume(&o.value,10);consume(&o.sw,2);consume(&o.cw,2);
   }
  }
 }
 __asm__ volatile("fldcw %0;ldmxcsr %1"::"m"(oldcw),"m"(oldmx));
 fprintf(out,"count=%u failures=%u result_state_hash=%016llx complete=1\n",count,fail,(unsigned long long)hash);
 fclose(out);return fail?1:0;
}
