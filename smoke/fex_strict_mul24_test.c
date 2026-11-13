#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "StrictMul24.h"
static uint32_t rng=0xa341316c;
static uint32_t next(void){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng;}
static uint64_t u64(void){return ((uint64_t)next()<<32)|next();}
static extFloat80_t make(uint64_t sig,uint16_t exp){extFloat80_t a={0};a.signif=sig;a.signExp=exp;return a;}
int main(int argc,char**argv){
 FILE*out=argc>1?fopen(argv[1],"wb"):stdout;if(!out)return 2;
 uint64_t checked=0,hit=0,rejected=0,native=0;unsigned failures=0;
 for(unsigned i=0;i<2400000;i++){
  uint64_t as=u64()|UINT64_C(0x8000000000000000),bs=u64()|UINT64_C(0x8000000000000000);
  uint16_t ae=(uint16_t)next(),be=(uint16_t)next();
  if(i%4!=0){as&=UINT64_C(0xffffff0000000000);bs&=UINT64_C(0xffffff0000000000);}
  if(i%16==0){as=u64();bs=u64();}
  if(i%32==1){ae=(uint16_t)(1+(next()%32766));be=(uint16_t)(1+(next()%32766));}
  if(i%32==2){ae=0;as=0;}
  if(i%32==3){ae=0x7fff;}
  if(i%32==4){as=UINT64_C(0x8000000000000000);bs=UINT64_C(0xffffff0000000000);ae=0x3fff;be=0x3fff;}
  if(i%32==5){ae=1;be=0x3ffe;}
  if(i%32==6){ae=0x7ffe;be=0x4000;}
  unsigned pc=i%12,rc=(i/12)%4;
  if(i%64==7 || i%64==8){
   as=(uint64_t)(i%64==7?8388609:8388611)<<40;bs=(uint64_t)12582912<<40;
   ae=be=0x3fff;pc=0;rc=0;
  }
  if(i%64==9 || i%64==10 || i%64==11){
   as=(uint64_t)8388609<<40;bs=(uint64_t)16777214<<40;
   ae=i%64==9?0x3fff:i%64==10?1:0x7ffe;be=i%64==9?0x3fff:i%64==10?0x3ffe:0x3fff;
   pc=0;rc=0;
  }
  struct softfloat_state state={0};state.detectTininess=(uint8_t)(i&1);
  state.roundingPrecision=pc<8?32:pc<10?64:80;state.roundingMode=rc==0?softfloat_round_near_even:rc==1?softfloat_round_min:rc==2?softfloat_round_max:softfloat_round_minMag;
  state.exceptionFlags=(uint8_t)(next()&0x3f);
  const struct softfloat_state before=state;struct softfloat_state ref=state;
  uint64_t sig=0x1122334455667788ULL;uint16_t exp=0xa55a;
  int used=FEXTryStrictMul24(&state,as,ae,bs,be,&sig,&exp);
  extFloat80_t expected=extF80_mul(&ref,make(as,ae),make(bs,be));
  if(used){
   hit++;
   if(sig!=expected.signif || exp!=expected.signExp || memcmp(&state,&ref,sizeof(state)))failures++;
#if defined(__i386__) || defined(__x86_64__)
   if(native<20000){
    extFloat80_t a=make(as,ae),b=make(bs,be),result={0};uint16_t savedcw,cw=0x7f,sw;
    __asm__ volatile("fnstcw %0;fldcw %3;fnclex;fldt %4;fldt %5;fmulp;fstpt %1;fnstsw %2;fldcw %0"
      :"=m"(savedcw),"=m"(result),"=m"(sw):"m"(cw),"m"(a),"m"(b):"st","st(1)");
    struct softfloat_state flags=before;flags.exceptionFlags=0;uint64_t ns;uint16_t ne;
    if(!FEXTryStrictMul24(&flags,as,ae,bs,be,&ns,&ne) || result.signif!=sig || result.signExp!=exp || !!(sw&32)!=!!(flags.exceptionFlags&softfloat_flag_inexact))failures++;
    native++;
   }
#endif
  }else{
   rejected++;
   if(memcmp(&state,&before,sizeof(state)) || sig!=0x1122334455667788ULL || exp!=0xa55a)failures++;
  }
  checked++;
 }
 fprintf(out,"checked=%llu hit=%llu rejected=%llu native_x87=%llu failures=%u complete=1\n",(unsigned long long)checked,(unsigned long long)hit,(unsigned long long)rejected,(unsigned long long)native,failures);
 if(out!=stdout)fclose(out);
 return failures?1:0;
}
