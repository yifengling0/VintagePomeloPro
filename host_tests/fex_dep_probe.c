#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef LONG (WINAPI *query_fn)(HANDLE,int,void*,ULONG,ULONG*);
typedef LONG (WINAPI *set_fn)(HANDLE,int,void*,ULONG);
typedef int (__cdecl *code_fn)(void);
static BYTE *landing,*tested;
static volatile int faults;
static int failures;
static LONG CALLBACK on_fault(EXCEPTION_POINTERS *p) {
  if (p->ExceptionRecord->ExceptionCode!=EXCEPTION_ACCESS_VIOLATION ||
      p->ExceptionRecord->NumberParameters<2 ||
      (BYTE*)p->ExceptionRecord->ExceptionInformation[1]!=tested) return EXCEPTION_CONTINUE_SEARCH;
  ++faults;
#ifdef _WIN64
  p->ContextRecord->Rip=(uintptr_t)landing;
#else
  p->ContextRecord->Eip=(uintptr_t)landing;
#endif
  return EXCEPTION_CONTINUE_EXECUTION;
}
static void code(BYTE *p,int n) { p[0]=0xb8; memcpy(p+1,&n,4);p[5]=0xc3; }
int main(int argc,char **argv) {
  FILE *out=fopen(argc>1?argv[1]:"dep-result.txt","w");if(!out)return 2;
  query_fn query=(query_fn)GetProcAddress(GetModuleHandleA("ntdll.dll"),"NtQueryInformationProcess");
  set_fn set=(set_fn)GetProcAddress(GetModuleHandleA("ntdll.dll"),"NtSetInformationProcess");
  if(!query||!set)return 3;
  AddVectoredExceptionHandler(1,on_fault);
  landing=VirtualAlloc(NULL,4096,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE);
  tested=VirtualAlloc(NULL,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
  if(!landing||!tested)return 4;
  code(landing,-777);code(tested,42);DWORD old;
  VirtualProtect(landing,4096,PAGE_EXECUTE_READ,&old);
  ULONG flags=0;LONG st=query(GetCurrentProcess(),34,&flags,sizeof(flags),NULL);
  fprintf(out,"initial query=%08lx flags=%lx bits=%u\n",st,flags,(unsigned)(sizeof(void*)*8));fflush(out);
  ULONG policies[]={2,1,2};
  for(unsigned i=0;i<3;i++) {
    ULONG policy=policies[i];st=set(GetCurrentProcess(),34,&policy,sizeof(policy));
    flags=0;LONG qs=query(GetCurrentProcess(),34,&flags,sizeof(flags),NULL);
    fprintf(out,"policy=%lu set=%08lx query=%08lx flags=%lx\n",policy,st,qs,flags);fflush(out);
    faults=0;int result=((code_fn)tested)();MEMORY_BASIC_INFORMATION info;
    VirtualQuery(tested,&info,sizeof(info));
    int allowed=(qs==0 && (flags&3)==2);
    if(result!=(allowed?42:-777) || faults!=(allowed?0:1) || info.Protect!=PAGE_READWRITE) ++failures;
    fprintf(out,"execute result=%d faults=%d protect=%lx state=%lx\n",result,faults,info.Protect,info.State);fflush(out);
    if(policy==2&&!st) {
      code(tested,99);faults=0;result=((code_fn)tested)();
      if(result!=99 || faults!=0) ++failures;
      fprintf(out,"native update result=%d faults=%d\n",result,faults);fflush(out);
      code(tested,42);
    }
  }
  fprintf(out,"RESULT failures=%d\n",failures);fclose(out);return failures?1:0;
}
