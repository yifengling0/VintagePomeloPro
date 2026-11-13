/* Native ARM64 watchdog: one stuck PE32 case must not block the suite. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <wchar.h>

int main(void)
{
    FILE *out=fopen("C:\\windows\\temp\\vp-wow64-context-runner.txt","w");if(!out)return 2;
    setvbuf(out,NULL,_IONBF,0);int failures=0;
    wchar_t probe[32768];DWORD length=GetModuleFileNameW(NULL,probe,32768);
    wchar_t *end=wcsrchr(probe,L'\\');
    if(!length || length>=32768 || !end){fprintf(out,"FAIL executable path\n");fclose(out);return 2;}
    wcscpy(end+1,L"wow64_context_probe.exe");
    for(int mode=0;mode<8;mode++) {
        wchar_t command[32768];
        if(swprintf(command,32768,L"\"%ls\" %d",probe,mode)<0){failures++;continue;}
        STARTUPINFOW si={0};si.cb=sizeof(si);PROCESS_INFORMATION pi={0};
        BOOL ok=CreateProcessW(probe,command,NULL,NULL,FALSE,0,NULL,NULL,&si,&pi);
        fprintf(out,"CREATE mode=%d ok=%u error=%lu pid=%lu\n",mode,ok,GetLastError(),pi.dwProcessId);
        if(!ok){failures++;continue;}
        DWORD wait=WaitForSingleObject(pi.hProcess,6000),code=~0u;
        BOOL queried=GetExitCodeProcess(pi.hProcess,&code);
        fprintf(out,"RESULT mode=%d wait=%lu exit=%lu\n",mode,wait,code);
        if(wait!=WAIT_OBJECT_0 || !queried || code!=0)failures++;
        if(wait!=WAIT_OBJECT_0){TerminateProcess(pi.hProcess,124);WaitForSingleObject(pi.hProcess,2000);}
        CloseHandle(pi.hThread);CloseHandle(pi.hProcess);
    }
    fprintf(out,"%s failures=%d\n",failures?"FAIL":"PASS",failures);fclose(out);return failures?1:0;
}
