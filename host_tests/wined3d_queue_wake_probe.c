/* OHOS Wine queue wake regression probe (Win32, no GPU or game required).
 * Build with llvm-mingw i686-w64-mingw32-clang -O2; also test ARM64.
 * Default: 20,000 auto-reset event queue handoffs, nonzero on any stall.
 * --compare-alert: also diagnose the thread-ID alert backend. A stall is
 * reported before one recovery wake; recovery never turns a failure into pass.
 * Bounded to 15 seconds per backend and a 45-second process watchdog.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
typedef LONG (WINAPI *alert_fn)(HANDLE);
typedef LONG (WINAPI *wait_fn)(const void*,const LARGE_INTEGER*);
static alert_fn alert_thread;
static wait_fn wait_alert;
static volatile LONG head,tail,waiting,stop;
static HANDLE event;
static DWORD worker_id;
static int events;
static volatile LONG waits,alerts;
static DWORD WINAPI worker(void* unused){
 (void)unused;
 while(!stop){
  if(head!=tail){InterlockedExchange(&tail,head);continue;}
  InterlockedExchange(&waiting,1);
  if(head!=tail&&InterlockedCompareExchange(&waiting,0,1))continue;
  InterlockedIncrement(&waits);
  if(events)WaitForSingleObject(event,INFINITE);else wait_alert(NULL,NULL);
 }
 return 0;
}
static DWORD WINAPI watchdog(void* unused){(void)unused;Sleep(45000);puts("WATCHDOG_TIMEOUT");fflush(stdout);TerminateProcess(GetCurrentProcess(),124);return 0;}
int main(int argc,char **argv){
 int first=(argc>1&&!strcmp(argv[1],"--compare-alert"))?0:1;
 unsigned failures=0;
 HMODULE ntdll=GetModuleHandleA("ntdll.dll");
 alert_thread=(alert_fn)GetProcAddress(ntdll,"NtAlertThreadByThreadId");
 wait_alert=(wait_fn)GetProcAddress(ntdll,"NtWaitForAlertByThreadId");
 if(!alert_thread||!wait_alert)return 2;
 HANDLE guard=CreateThread(NULL,0,watchdog,NULL,0,NULL);if(guard)CloseHandle(guard);
 for(events=first;events<2;events++){
  head=tail=waiting=stop=waits=alerts=0;event=CreateEventA(NULL,FALSE,FALSE,NULL);
  HANDLE thread=CreateThread(NULL,0,worker,NULL,0,&worker_id);if(!thread)return 3;
  DWORD started=GetTickCount(),stalls=0,recovered=0;
  for(LONG i=1;i<=20000;i++){
   InterlockedExchange(&head,i);
   if(InterlockedCompareExchange(&waiting,0,1)){
    InterlockedIncrement(&alerts);
    if(events)SetEvent(event);else alert_thread((HANDLE)(ULONG_PTR)worker_id);
   }
   DWORD deadline=GetTickCount()+250;
   while(tail!=i&&(LONG)(GetTickCount()-deadline)<0)Sleep(0);
   if(tail!=i){
    ++stalls;printf("STALL mode=%s iteration=%ld head=%ld tail=%ld waiting=%ld waits=%ld alerts=%ld\n",events?"event":"alert",i,head,tail,waiting,waits,alerts);fflush(stdout);
    if(events)SetEvent(event);else alert_thread((HANDLE)(ULONG_PTR)worker_id);
    deadline=GetTickCount()+1000;while(tail!=i&&(LONG)(GetTickCount()-deadline)<0)Sleep(0);
    if(tail==i)++recovered;
    if(stalls>=4||tail!=i)break;
   }
   if(GetTickCount()-started>15000)break;
  }
  stop=1;if(events)SetEvent(event);else alert_thread((HANDLE)(ULONG_PTR)worker_id);
  DWORD ended=WaitForSingleObject(thread,2000);
  printf("RESULT mode=%s head=%ld tail=%ld waits=%ld alerts=%ld stalls=%lu recovered=%lu elapsed=%lu joined=%lu\n",events?"event":"alert",head,tail,waits,alerts,stalls,recovered,GetTickCount()-started,ended);fflush(stdout);
  if(ended!=WAIT_OBJECT_0)return 4;
  if(stalls || tail!=20000)++failures;
  CloseHandle(thread);CloseHandle(event);
 }
 puts("COMPLETE");return failures?1:0;
}
