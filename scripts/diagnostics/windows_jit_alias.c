/* SPDX-License-Identifier: MIT */
/* Compare the Windows CoreCLR-style dual view with private executable memory. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

typedef unsigned (__cdecl *generated_code)(void);

static int api_error(FILE *log, const char *mode, const char *api)
{
    DWORD error = GetLastError();
    fprintf(log, "mode=%s api=%s error=%lu\n", mode, api, (unsigned long)error);
    return 1;
}

static int exercise(FILE *log, const char *mode, unsigned char *rx, unsigned char *rw)
{
    static const unsigned initial = 0xf0f0f0f0;
    unsigned visible;

    /* Match a write-barrier immediate and verify it before the first execution. */
    rw[0] = 0xb8; /* mov eax, imm32 */
    memcpy(rw + 1, &initial, sizeof(initial));
    rw[5] = 0xc3; /* ret */
    memcpy(&visible, rx + 1, sizeof(visible));
    if (rx[0] != 0xb8 || visible != initial || rx[5] != 0xc3)
    {
        fprintf(log, "mode=%s phase=visibility result=fail value=0x%08x\n", mode, visible);
        return 1;
    }
    fprintf(log, "mode=%s phase=visibility result=pass\n", mode);

    generated_code execute = (generated_code)(uintptr_t)rx;
    if (!FlushInstructionCache(GetCurrentProcess(), rx, 6))
        return api_error(log, mode, "FlushInstructionCache");
    if (execute() != initial)
    {
        fprintf(log, "mode=%s phase=first-execution result=fail\n", mode);
        return 1;
    }
    fprintf(log, "mode=%s phase=first-execution result=pass\n", mode);

    for (unsigned expected = 1; expected <= 512; ++expected)
    {
        memcpy(rw + 1, &expected, sizeof(expected));
        memcpy(&visible, rx + 1, sizeof(visible));
        if (visible != expected)
        {
            fprintf(log, "mode=%s phase=data-update result=fail expected=%u actual=%u\n",
                    mode, expected, visible);
            return 1;
        }
        if (!FlushInstructionCache(GetCurrentProcess(), rx, 6))
            return api_error(log, mode, "FlushInstructionCache");
        unsigned actual = execute();
        if (actual != expected)
        {
            fprintf(log, "mode=%s phase=code-update result=fail expected=%u actual=%u\n",
                    mode, expected, actual);
            return 1;
        }
    }
    fprintf(log, "mode=%s phase=code-update result=pass count=512\n", mode);
    return 0;
}

static int dual_view(FILE *log, DWORD offset, DWORD page, const char *mode)
{
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL,
                                        PAGE_EXECUTE_READWRITE | SEC_RESERVE, 0, offset + page, NULL);
    if (!section) return api_error(log, mode, "CreateFileMappingW");

    int result;
    unsigned char *rw = NULL;
    unsigned char *rx = MapViewOfFile(section, FILE_MAP_EXECUTE | FILE_MAP_READ | FILE_MAP_WRITE,
                                      0, offset, page);
    if (!rx) result = api_error(log, mode, "MapViewOfFile(exec)");
    else if (!VirtualAlloc(rx, page, MEM_COMMIT, PAGE_EXECUTE_READ))
        result = api_error(log, mode, "VirtualAlloc(commit-exec)");
    else if (!(rw = MapViewOfFile(section, FILE_MAP_READ | FILE_MAP_WRITE, 0, offset, page)))
        result = api_error(log, mode, "MapViewOfFile(write)");
    else
    {
        MEMORY_BASIC_INFORMATION info;
        if (!VirtualQuery(rx, &info, sizeof(info))) result = api_error(log, mode, "VirtualQuery");
        else
        {
            fprintf(log, "mode=%s offset=%lu exec_protect=0x%lx distinct=%d\n",
                    mode, (unsigned long)offset, (unsigned long)info.Protect, rx != rw);
            result = exercise(log, mode, rx, rw);
        }
    }
    if (rw) UnmapViewOfFile(rw);
    if (rx) UnmapViewOfFile(rx);
    CloseHandle(section);
    return result;
}

static int protect_shared_view(FILE *log, DWORD page)
{
    const char *mode = "shared-add-exec";
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL,
                                        PAGE_EXECUTE_READWRITE | SEC_RESERVE, 0, page, NULL);
    if (!section) return api_error(log, mode, "CreateFileMappingW");

    int result;
    volatile unsigned char *rx = MapViewOfFile(section, FILE_MAP_EXECUTE | FILE_MAP_READ | FILE_MAP_WRITE,
                                               0, 0, page);
    volatile unsigned char *rw = NULL;
    if (!rx) result = api_error(log, mode, "MapViewOfFile(exec)");
    else if (!VirtualAlloc((void *)rx, page, MEM_COMMIT, PAGE_READWRITE))
        result = api_error(log, mode, "VirtualAlloc(commit-write)");
    else if (!(rw = MapViewOfFile(section, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, page)))
        result = api_error(log, mode, "MapViewOfFile(write)");
    else
    {
        DWORD previous;
        rw[0] = 0x39;
        unsigned before = rx[0];
        if (!VirtualProtect((void *)rx, page, PAGE_EXECUTE_READ, &previous))
            result = api_error(log, mode, "VirtualProtect(add-exec)");
        else
        {
            rw[0] = 0x73;
            unsigned after = rx[0];
            fprintf(log, "mode=%s before_exec=0x%02x after_exec=0x%02x expected_after=0x73\n",
                    mode, before, after);
            result = before != 0x39 || after != 0x73;
        }
    }
    if (rw) UnmapViewOfFile((void *)rw);
    if (rx) UnmapViewOfFile((void *)rx);
    CloseHandle(section);
    return result;
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR arguments, int show)
{
    (void)instance;
    (void)previous;
    (void)arguments;
    (void)show;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    WCHAR path[4096];
    DWORD len = GetModuleFileNameW(NULL, path, sizeof(path) / sizeof(path[0]));
    if (!len || len >= sizeof(path) / sizeof(path[0]) - 4) return 2;
    wcscpy(path + len, L".log");
    FILE *log = _wfopen(path, L"w");
    if (!log) return 3;
    setvbuf(log, NULL, _IONBF, 0);
    SYSTEM_INFO system;
    GetSystemInfo(&system);
    fprintf(log, "windows_jit_alias bits=%u page=%lu granularity=%lu\n",
            (unsigned)(sizeof(void *) * 8), (unsigned long)system.dwPageSize,
            (unsigned long)system.dwAllocationGranularity);

    unsigned char *single = VirtualAlloc(NULL, system.dwPageSize,
                                         MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    int baseline = single ? exercise(log, "private-rwx", single, single) :
                            api_error(log, "private-rwx", "VirtualAlloc");
    if (single) VirtualFree(single, 0, MEM_RELEASE);
    int first = dual_view(log, 0, system.dwPageSize, "dual-zero");
    int second = dual_view(log, system.dwAllocationGranularity, system.dwPageSize, "dual-offset");
    int protect = protect_shared_view(log, system.dwPageSize);
    fprintf(log, "summary baseline=%d dual_zero=%d dual_offset=%d add_exec=%d\n",
            baseline, first, second, protect);
    fclose(log);
    return baseline || first || second || protect;
}
