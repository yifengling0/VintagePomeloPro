/* Real-device Broker v2 startup contract: parent launch and CreateProcess child. */
#include "../common/winehua_t_check.h"
#include <stdlib.h>
#include <wchar.h>

static int check_environment(void)
{
    WCHAR name[40], *value = malloc(16385 * sizeof(WCHAR));
    WCHAR text[64], empty[2];
    unsigned int i, j;
    int ok = value != NULL;
    if (!value) return 0;
    for (i = 0; i < 4; ++i)
    {
        wsprintfW(name, L"VPP_BROKER_BULK%u", i);
        if (GetEnvironmentVariableW(name, value, 16385) != 16384) { ok = 0; continue; }
        for (j = 0; j < 16384; ++j) if (value[j] != L'A' + i) { ok = 0; break; }
    }
    free(value);
    if (!GetEnvironmentVariableW(L"VPP_BROKER_TEXT", text, 64) ||
        wcscmp(text, L"\x6d4b\x8bd5|line\nnext=tail")) ok = 0;
    SetLastError(ERROR_SUCCESS);
    if (GetEnvironmentVariableW(L"VPP_BROKER_EMPTY", empty, 2) != 0 ||
        GetLastError() == ERROR_ENVVAR_NOT_FOUND) ok = 0;
    return ok;
}

int main(int argc, char **argv)
{
    WCHAR path[4096], command[4096];
    STARTUPINFOW si = {0};
    PROCESS_INFORMATION pi = {0};
    DWORD exit_code = 0, wait_rc;
    int i, marker = -1, args_ok = 1;
    char expected[32];
    if (argc > 1 && !strcmp(argv[1], "--contract-child"))
        return argc == 6 && !strcmp(argv[2], "") && !strcmp(argv[3], "pipe|value") &&
            !strcmp(argv[4], "line\nbreak") && !strcmp(argv[5], "__winehua_desktop__") &&
            check_environment() ? 42 : 43;
    t_begin("broker-contract", argc, argv);
    for (i = 1; i < argc; ++i) if (!strcmp(argv[i], "--contract-args")) { marker = i; break; }
    if (marker < 0 || argc != marker + 256) args_ok = 0;
    else for (i = 1; i < 256; ++i)
    {
        const char *want;
        snprintf(expected, sizeof(expected), "value-%03d", i);
        want = i == 1 ? "" : i == 2 ? "pipe|value" : i == 3 ? "line\nbreak" :
            i == 4 ? "__winehua_desktop__" : expected;
        if (strcmp(argv[marker + i], want)) args_ok = 0;
    }
    t_check("parent-256-arguments", args_ok, "argc=%d marker=%d", argc, marker);
    t_check("parent-64k-environment", check_environment(), "4 x 16384 WCHAR values, UTF-8 and empty value");
    t_metric("parent_argc", "%d", argc);
    t_metric("bulk_environment_characters", "%d", 65536);
    si.cb = sizeof(si);
    if (!GetModuleFileNameW(NULL, path, 4096) ||
        swprintf(command, 4096, L"\"%ls\" --contract-child \"\" \"pipe|value\" \"line\nbreak\" __winehua_desktop__", path) < 0)
    {
        t_check("self-command", 0, "cannot construct child command");
        return t_finish();
    }
    if (!CreateProcessW(NULL, command, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
    {
        t_check("child-create", 0, "error=%lu", GetLastError());
        return t_finish();
    }
    wait_rc = WaitForSingleObject(pi.hProcess, 30000);
    t_check("child-wait", wait_rc == WAIT_OBJECT_0, "wait=%lu", wait_rc);
    if (wait_rc != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 44);
    if (!GetExitCodeProcess(pi.hProcess, &exit_code)) exit_code = 45;
    t_check("child-contract", exit_code == 42, "exit=%lu", exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return t_finish();
}
