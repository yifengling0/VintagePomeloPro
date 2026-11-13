#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef int (__cdecl *code_fn)(void);
static BYTE *page;
static volatile int write_avs, read_avs, execute_avs, guard_avs;
static int failures;
static volatile int handling_write, handling_guard, handling_invalid;
static volatile int invalid_exceptions;
static DWORD invalid_status;
static LONG CALLBACK handler(EXCEPTION_POINTERS *p) {
  if (handling_invalid &&
      (p->ExceptionRecord->ExceptionCode == EXCEPTION_ILLEGAL_INSTRUCTION ||
       p->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) &&
      (BYTE*)p->ExceptionRecord->ExceptionAddress >= page &&
      (BYTE*)p->ExceptionRecord->ExceptionAddress < page + 32) {
    ++invalid_exceptions; invalid_status = p->ExceptionRecord->ExceptionCode;
#ifdef _WIN64
    p->ContextRecord->Rip = (uintptr_t)(page + 128);
#else
    p->ContextRecord->Eip = (uintptr_t)(page + 128);
#endif
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  if (p->ExceptionRecord->ExceptionCode==STATUS_GUARD_PAGE_VIOLATION && handling_guard) {
    ++guard_avs; return EXCEPTION_CONTINUE_EXECUTION;
  }
  if (p->ExceptionRecord->ExceptionCode==EXCEPTION_ACCESS_VIOLATION && p->ExceptionRecord->NumberParameters>=2) {
    if (p->ExceptionRecord->ExceptionInformation[0]==1) {
      ++write_avs;
      if (handling_write && (BYTE*)p->ExceptionRecord->ExceptionInformation[1]>=page &&
          (BYTE*)p->ExceptionRecord->ExceptionInformation[1]<page+4096) {
        DWORD old; if (VirtualProtect(page,4096,PAGE_EXECUTE_READWRITE,&old)) return EXCEPTION_CONTINUE_EXECUTION;
      }
    } else if(p->ExceptionRecord->ExceptionInformation[0]==0) ++read_avs;
    else if(p->ExceptionRecord->ExceptionInformation[0]==8) ++execute_avs;
  }
  return EXCEPTION_CONTINUE_SEARCH;
}
static void check(FILE *out,const char *name,int pass) {
  fprintf(out,"%s %s\n",pass?"PASS":"FAIL",name); fflush(out); if(!pass)++failures;
}
static void put_code(BYTE *p,int value) {
  p[0]=0xb8; memcpy(p+1,&value,4); p[5]=0xc3;
}
struct native_writer {
  BYTE *code;
  HANDLE input, ready, finished;
  volatile LONG failures;
};
static DWORD WINAPI native_writer_thread(void *opaque) {
  struct native_writer *writer = opaque;
  for (int i = 0; i < 32; ++i) {
    if (WaitForSingleObject(writer->ready, 5000) != WAIT_OBJECT_0) {
      InterlockedIncrement(&writer->failures); return 1;
    }
    DWORD received = 0;
    // ReadFile writes through Wine's native NT path, outside translated x86.
    if (!ReadFile(writer->input, writer->code, 6, &received, NULL) || received != 6)
      InterlockedIncrement(&writer->failures);
    SetEvent(writer->finished);
  }
  return 0;
}
int main(int argc,char **argv) {
  FILE *out=fopen(argc>1?argv[1]:"vm-contract-result.txt","w");
  if(!out)return 2;
  AddVectoredExceptionHandler(1,handler);
  page=VirtualAlloc(NULL,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
  if(!page)return 3;
  DWORD old; MEMORY_BASIC_INFORMATION info;
  put_code(page,42); VirtualProtect(page,4096,PAGE_EXECUTE_READ,&old); FlushInstructionCache(GetCurrentProcess(),page,6);
  check(out,"RW-to-RX generated code",((code_fn)page)()==42);
  VirtualProtect(page,4096,PAGE_READWRITE,&old); put_code(page,99);
  VirtualProtect(page,4096,PAGE_EXECUTE_READ,&old); FlushInstructionCache(GetCurrentProcess(),page,6);
  check(out,"replaced code after FlushInstructionCache",((code_fn)page)()==99);
  VirtualProtect(page,4096,PAGE_EXECUTE_READWRITE,&old); ((code_fn)page)();
  VirtualQuery(page,&info,sizeof(info));
  check(out,"VirtualQuery retains guest RWX after execution",info.Protect==PAGE_EXECUTE_READWRITE);
  VirtualProtect(page,4096,PAGE_EXECUTE_READ,&old);
  check(out,"VirtualProtect returns logical guest OldProtect",old==PAGE_EXECUTE_READWRITE);
  handling_write=1;write_avs=0;*((volatile BYTE*)page+64)=123;handling_write=0;
  check(out,"guest RX write delivered to VEH exactly once",write_avs==1 && page[64]==123);
  VirtualProtect(page,4096,PAGE_EXECUTE_READWRITE|PAGE_GUARD,&old);
  handling_guard=1;guard_avs=0;volatile BYTE value=page[64];(void)value;handling_guard=0;
  check(out,"guard exception preserved",guard_avs==1);
  VirtualQuery(page,&info,sizeof(info));
  check(out,"guard cleared after first use",!(info.Protect&PAGE_GUARD));
  // An out-of-allocation protect must fail; not silently truncate to valid pages.
  BYTE *reservation=VirtualAlloc(NULL,8192,MEM_RESERVE,PAGE_NOACCESS);
  VirtualAlloc(reservation,4096,MEM_COMMIT,PAGE_READWRITE);
  BOOL ok=VirtualProtect(reservation,8192,PAGE_EXECUTE_READ,&old);
  check(out,"protect spanning uncommitted page fails",!ok);
  VirtualQuery(reservation,&info,sizeof(info));
  check(out,"failed protect leaves first page unchanged",info.Protect==PAGE_READWRITE);
  VirtualFree(reservation,0,MEM_RELEASE);
  // Inline SMC changes an immediate in the next instruction on the same page.
  VirtualProtect(page,4096,PAGE_EXECUTE_READWRITE,&old);
  // Use a register target on both ISAs; this also exercises an in-flight
  // writer whose next instruction is changed before its first execution.
#ifdef _WIN64
  page[0]=0x48;page[1]=0xba;uint64_t target=(uintptr_t)(page+14);
  memcpy(page+2,&target,8);page[10]=0xc6;page[11]=0x02;page[12]=7;
  put_code(page+13,1);
#else
  page[0]=0xba;uint32_t target=(uintptr_t)(page+9);
  memcpy(page+1,&target,4);page[5]=0xc6;page[6]=0x02;page[7]=7;
  put_code(page+8,1);
#endif
  FlushInstructionCache(GetCurrentProcess(),page,32);
  check(out,"first inline self-modification executes new immediate",((code_fn)page)()==7);
  check(out,"modified code remains current on second call",((code_fn)page)()==7);
  // A native/API writer must be visible without relying on a guest JIT
  // write fault. Restore RX, then legal RWX, after its first execution.
  put_code(page,12);FlushInstructionCache(GetCurrentProcess(),page,6);
  check(out,"native writer publishes initial RWX code",((code_fn)page)()==12);
  put_code(page,31);
  check(out,"native writer updates cached RWX code",((code_fn)page)()==31);
  VirtualProtect(page,4096,PAGE_EXECUTE_READ,&old);
  check(out,"restored RX executes validated code",((code_fn)page)()==31);
  VirtualFree(page,0,MEM_RELEASE);
  // One instruction has bytes in an RX page and a writable executable page.
  page=VirtualAlloc(NULL,8192,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE);
  BYTE *cross=page+4094;put_code(cross,23<<8);
  VirtualProtect(page,4096,PAGE_EXECUTE_READ,&old);
  FlushInstructionCache(GetCurrentProcess(),cross,6);
  check(out,"mixed-permission cross-page instruction",((code_fn)cross)()==(23<<8));
  cross[2]=41;
  check(out,"cross-page instruction notices writable-byte update",((code_fn)cross)()==(41<<8));
  VirtualFree(page,0,MEM_RELEASE);
  // Native API writes into already translated code, synchronized with the
  // reader so this tests publication, not an application-level data race.
  page = VirtualAlloc(NULL,4096,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE);
  put_code(page,5); FlushInstructionCache(GetCurrentProcess(),page,6);
  int native_ok = ((code_fn)page)() == 5;
  HANDLE pipe_input = NULL, pipe_output = NULL;
  struct native_writer writer = {0}; writer.code = page;
  native_ok &= CreatePipe(&pipe_input,&pipe_output,NULL,0) != 0;
  writer.input = pipe_input;
  writer.ready = CreateEvent(NULL,FALSE,FALSE,NULL);
  writer.finished = CreateEvent(NULL,FALSE,FALSE,NULL);
  HANDLE thread = native_ok && writer.ready && writer.finished ?
      CreateThread(NULL,0,native_writer_thread,&writer,0,NULL) : NULL;
  native_ok &= thread != NULL;
  if (thread) {
    for (int i = 0; i < 32; ++i) {
      BYTE next[6]; DWORD written = 0; put_code(next,100+i);
      if (!WriteFile(pipe_output,next,6,&written,NULL) || written != 6) { native_ok=0; break; }
      SetEvent(writer.ready);
      if (WaitForSingleObject(writer.finished,5000) != WAIT_OBJECT_0) { native_ok=0; break; }
      native_ok &= ((code_fn)page)() == 100+i;
    }
    native_ok &= WaitForSingleObject(thread,5000) == WAIT_OBJECT_0 && !writer.failures;
    if (!native_ok && WaitForSingleObject(thread,0) != WAIT_OBJECT_0) {
      check(out,"native API writer timeout",0); fclose(out); return 4;
    }
    CloseHandle(thread);
  }
  if (writer.ready) CloseHandle(writer.ready);
  if (writer.finished) CloseHandle(writer.finished);
  if (pipe_input) CloseHandle(pipe_input);
  if (pipe_output) CloseHandle(pipe_output);
  check(out,"cross-thread native ReadFile code publication",native_ok);
  // Decommit/recommit retains the reservation but ends the code lifetime.
  BYTE *same = page;
  int reused = VirtualFree(page,4096,MEM_DECOMMIT) != 0;
  reused &= VirtualAlloc(same,4096,MEM_COMMIT,PAGE_READWRITE) == same;
  if (reused) {
    put_code(same,211); VirtualProtect(same,4096,PAGE_EXECUTE_READ,&old);
    FlushInstructionCache(GetCurrentProcess(),same,6);
    reused &= ((code_fn)same)() == 211;
  }
  check(out,"decommit and recommit discard previous code",reused);
  reused = VirtualFree(same,0,MEM_RELEASE) != 0;
  page = VirtualAlloc(same,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
  reused &= page == same;
  if (reused) {
    put_code(page,217); VirtualProtect(page,4096,PAGE_EXECUTE_READ,&old);
    FlushInstructionCache(GetCurrentProcess(),page,6);
    reused &= ((code_fn)page)() == 217;
  }
  check(out,"release and remap discard previous code",reused);
  if (page) VirtualFree(page,0,MEM_RELEASE);
  // Prefixes plus a 32-bit immediate exceed the architectural 15-byte limit.
  page=VirtualAlloc(NULL,4096,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE);
  memset(page,0x66,14); put_code(page+14,9); page[128]=0xc3;
  FlushInstructionCache(GetCurrentProcess(),page,129);
  handling_invalid=1; invalid_exceptions=0; ((code_fn)page)(); handling_invalid=0;
  check(out,"overlong instruction returns guest exception",invalid_exceptions==1);
  fprintf(out,"INVALID status=%08lx count=%d\n",invalid_status,invalid_exceptions);
  VirtualFree(page,0,MEM_RELEASE);
  fprintf(out,"RESULT failures=%d write_avs=%d guard_avs=%d\n",failures,write_avs,guard_avs);
  fclose(out);return failures?1:0;
}
