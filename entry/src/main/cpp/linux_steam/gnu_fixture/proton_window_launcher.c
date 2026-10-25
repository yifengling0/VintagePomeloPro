#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
int main(void) {
    STARTUPINFOA startup = {0};
    PROCESS_INFORMATION child = {0};
    startup.cb = sizeof(startup);
    char command[] = "C:\\proton-x64-window-real.exe";
    SetEnvironmentVariableA("WINEDEBUG", "-all,+timestamp,+pid,+tid,+module,+server,+win32u,+waylanddrv,+seh");
    SetEnvironmentVariableA("STEAMOHOS_MODULE_DIAGNOSTICS", "1");
    if (!CreateProcessA(NULL, command, NULL, NULL, TRUE, 0, NULL, NULL, &startup, &child)) {
        fprintf(stderr, "window child error=%lu\n", GetLastError()); return 20;
    }
    DWORD status = WaitForSingleObject(child.hProcess, 30000), code = 21;
    if (status == WAIT_OBJECT_0) GetExitCodeProcess(child.hProcess, &code);
    else TerminateProcess(child.hProcess, 21);
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    return (int)code;
}
