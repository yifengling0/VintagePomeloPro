// Real PE32 JIT calls with two independent, changing precision controls.
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef struct { uint64_t sig; uint16_t exp; } __attribute__((packed)) F80;
typedef struct { F80 value; uint16_t sw,cw,pad; uint32_t mxcsr,flags,regs[8],xmm[32]; } Observation;
_Static_assert(sizeof(Observation)==184,"assembly layout");
uint32_t x87_patterns[32];
typedef void (*Probe)(const F80*,const F80*,uint32_t,Observation*);
extern void x87_add(const F80*,const F80*,uint32_t,Observation*);
extern void x87_sub(const F80*,const F80*,uint32_t,Observation*);
extern void x87_mul(const F80*,const F80*,uint32_t,Observation*);
extern void x87_div(const F80*,const F80*,uint32_t,Observation*);
static Probe probes[]={x87_add,x87_sub,x87_mul,x87_div};
static HANDLE gate;
struct Result { uint32_t id,count,fail; uint64_t hash; };
static void consume(struct Result *r,const void *p,size_t n) {
 const uint8_t *v=p;while(n--){r->hash^=*v++;r->hash*=UINT64_C(0x100000001b3);}
}
static DWORD WINAPI thread(void *ptr) {
 struct Result *r=ptr;
 const uint32_t expect[]={0x768696a6,0x65758595,0x54647484,0,0x43536373,0x32425262,0x21314151,0x10203040};
 WaitForSingleObject(gate,10000);
 for(unsigned i=0;i<8192;i++) {
  unsigned pc=(i+r->id)%3,rc=((i/3)+r->id)%4;
  uint32_t cw=0x7f|((pc==0?0:pc==1?2:3)<<8)|(rc<<10);
  F80 a={UINT64_C(0x8123450000000000)+(uint64_t)(i&0xffff)*UINT64_C(0x10000000000),0x3fff};
  F80 b={UINT64_C(0xf1abcd0000000000),0x3fc0+(i&127)};
  for(unsigned op=0;op<4;op++) {
   Observation o={0};probes[op](&a,&b,cw,&o);r->count++;
   if(o.cw!=cw || (o.flags&0x8d5)!=0x8d5 || memcmp(o.xmm,x87_patterns,sizeof(o.xmm)))r->fail++;
   for(unsigned g=0;g<8;g++)if(g!=3 && o.regs[g]!=expect[g])r->fail++;
   consume(r,&o.value,10);consume(r,&o.sw,2);consume(r,&o.cw,2);
  }
  if(!(i&63))Sleep(0);
 }
 return 0;
}
int main(int argc,char **argv) {
 FILE *out=argc>1?fopen(argv[1],"wb"):stdout;if(!out)return 2;
 for(unsigned i=0;i<32;i++)x87_patterns[i]=0x9e3779b9U*(i+1);
 gate=CreateEventW(NULL,TRUE,FALSE,NULL);if(!gate)return 3;
 struct Result r[2]={{0,0,0,UINT64_C(0xcbf29ce484222325)},{1,0,0,UINT64_C(0xcbf29ce484222325)}};
 HANDLE handles[2];
 for(unsigned i=0;i<2;i++){handles[i]=CreateThread(NULL,0,thread,&r[i],0,NULL);if(!handles[i])return 4;}
 SetEvent(gate);
 if(WaitForMultipleObjects(2,handles,TRUE,60000)!=WAIT_OBJECT_0)return 5;
 unsigned fail=0;
 for(unsigned i=0;i<2;i++) {
  fprintf(out,"thread=%u count=%u failures=%u result_state_hash=%016llx\n",i,r[i].count,r[i].fail,(unsigned long long)r[i].hash);
  fail+=r[i].fail;CloseHandle(handles[i]);
 }
 CloseHandle(gate);fprintf(out,"complete=1\n");if(out!=stdout)fclose(out);
 return fail?1:0;
}
