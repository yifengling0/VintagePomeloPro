#include <windows.h>
#include <stdint.h>
#include <stdio.h>
static uint64_t ticks(void){uint64_t v;__asm__ volatile("isb; mrs %0,cntvct_el0":"=r"(v)::"memory");return v;}
int main(int argc,char**argv){
 if(argc!=2)return 2;FILE*f=fopen(argv[1],"wb");if(!f)return 3;
 uint64_t freq;__asm__ volatile("mrs %0,cntfrq_el0":"=r"(freq));
 LARGE_INTEGER pf;QueryPerformanceFrequency(&pf);
 for(unsigned i=0;i<3;i++){
  LARGE_INTEGER a,b;QueryPerformanceCounter(&a);uint64_t t0=ticks();
  Sleep(200);uint64_t t1=ticks();QueryPerformanceCounter(&b);
  double seconds=(double)(b.QuadPart-a.QuadPart)/pf.QuadPart;
  fprintf(f,"cntfrq=%llu delta_ticks=%llu wall_seconds=%.6f inferred_hz=%.0f\n",(unsigned long long)freq,(unsigned long long)(t1-t0),seconds,(t1-t0)/seconds);
 }fputs("complete=1\n",f);fclose(f);return 0;
}
