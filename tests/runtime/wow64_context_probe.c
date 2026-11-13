/* Generic PE32 GetThreadContext regression for software WOW64 CPUs. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

typedef LONG (WINAPI *query_thread_fn)(HANDLE,ULONG,void*,ULONG,ULONG*);
static DWORD WINAPI worker(void *event) { return WaitForSingleObject(event,INFINITE); }

int main(int argc,char **argv)
{
    int mode=argc>1?atoi(argv[1]):-1;
    if(mode<0 || mode>7)return 2;
    char path[MAX_PATH];
    snprintf(path,sizeof(path),"C:\\windows\\temp\\vp-wow64-context-%d.txt",mode);
    FILE *out=fopen(path,"w");if(!out)return 2;setvbuf(out,NULL,_IONBF,0);
    HANDLE thread=GetCurrentThread(),event=NULL;DWORD tid=GetCurrentThreadId();int failed=0;
    if(mode==1 || mode==5 || mode==7) {
        DWORD access=mode==1?THREAD_GET_CONTEXT:mode==5?THREAD_QUERY_INFORMATION:THREAD_ALL_ACCESS;
        if(!DuplicateHandle(GetCurrentProcess(),thread,GetCurrentProcess(),&thread,access,FALSE,0))failed=1;
    }
    if(mode==2)thread=OpenThread(THREAD_ALL_ACCESS,FALSE,tid);
    if(mode==3 || mode==6) {
        event=CreateEventA(NULL,TRUE,FALSE,NULL);
        thread=event?CreateThread(NULL,0,worker,event,mode==3?CREATE_SUSPENDED:0,&tid):NULL;
    }
    if(mode==4)thread=(HANDLE)(ULONG_PTR)0xdeadbeef;
    if(!thread || failed){fprintf(out,"FAIL setup error=%lu\n",GetLastError());fclose(out);return 3;}
    HANDLE queryHandle=mode==4?NULL:OpenThread(THREAD_QUERY_INFORMATION,FALSE,tid);
    query_thread_fn query=(query_thread_fn)GetProcAddress(GetModuleHandleA("ntdll.dll"),"NtQueryInformationThread");
    ULONG before=~0u,after=~0u;
    LONG beforeStatus=query && queryHandle?query(queryHandle,35,&before,sizeof(before),NULL):-1;
    CONTEXT context={0};context.ContextFlags=CONTEXT_FULL|CONTEXT_DEBUG_REGISTERS;
    fprintf(out,"BEFORE mode=%d tid=%lu query=%08lx suspend=%lu\n",mode,tid,beforeStatus,before);
    SetLastError(0);BOOL ok=GetThreadContext(thread,&context);DWORD error=GetLastError();
    LONG afterStatus=query && queryHandle?query(queryHandle,35,&after,sizeof(after),NULL):-1;
    fprintf(out,"AFTER ok=%u error=%lu query=%08lx suspend=%lu flags=%08lx eip=%08lx\n",ok,error,afterStatus,after,context.ContextFlags,context.Eip);
    if(mode==4)failed=ok || error!=ERROR_INVALID_HANDLE;
    else if(mode==5)failed=ok || error!=ERROR_ACCESS_DENIED;
    else failed=!ok;
    if(mode!=4)failed|=beforeStatus!=0 || afterStatus!=0 || before!=after || before!=(mode==3?1u:0u);
    if(mode==3 || mode==6) {
        SetEvent(event);if(mode==3)ResumeThread(thread);
        DWORD wait=WaitForSingleObject(thread,2000);
        fprintf(out,"WORKER wait=%lu\n",wait);failed|=wait!=WAIT_OBJECT_0;
        if(wait!=WAIT_OBJECT_0)TerminateThread(thread,125);
        CloseHandle(event);
    }
    if(queryHandle)CloseHandle(queryHandle);
    if(mode!=0 && mode!=4)CloseHandle(thread);
    fprintf(out,"%s mode=%d\n",failed?"FAIL":"PASS",mode);fclose(out);return failed?1:0;
}
