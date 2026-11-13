/* Read-only component probe. Fixed inputs, bounded reads, no game/account files. */
#include <windows.h>
#include <bcrypt.h>
#ifdef WINE_EXIT_PROBE
#include <tlhelp32.h>
#endif
#include <stdio.h>
#include <string.h>

#ifdef WINE_COMPONENT_PROBE
#define PROBE_LOG L"wine-probe-"
#else
#define PROBE_LOG L"fex-probe-"
#endif

static HANDLE output;
static void emit(const char *s) { DWORD n; WriteFile(output, s, (DWORD)strlen(s), &n, NULL); }
static void hash_file(const char *label, const WCHAR *path) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    char line[256];
    if (file == INVALID_HANDLE_VALUE) { snprintf(line, sizeof line, "%s error=%lu\n", label, GetLastError()); emit(line); return; }
    LARGE_INTEGER size; GetFileSizeEx(file, &size);
    if (size.QuadPart < 0 || size.QuadPart > 16 * 1024 * 1024) { emit("file size rejected\n"); CloseHandle(file); return; }
    BCRYPT_ALG_HANDLE alg = NULL; BCRYPT_HASH_HANDLE hash = NULL;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0);
    if (status >= 0) status = BCryptCreateHash(alg, &hash, NULL, 0, NULL, 0, 0);
    unsigned char buffer[16384], digest[32]; DWORD n;
    while (status >= 0) {
        if (!ReadFile(file, buffer, sizeof buffer, &n, NULL)) { status=(NTSTATUS)0xc0000185; break; }
        if (!n) break;
        status = BCryptHashData(hash, buffer, n, 0);
    }
    if (status >= 0) status = BCryptFinishHash(hash, digest, sizeof digest, 0);
    snprintf(line, sizeof line, "%s bytes=%lld status=%lx sha256=", label, size.QuadPart, (unsigned long)status); emit(line);
    if (status >= 0) { for (int i=0; i<32; ++i) { snprintf(line, sizeof line, "%02x", digest[i]); emit(line); } }
    emit("\n"); if (hash) BCryptDestroyHash(hash); if (alg) BCryptCloseAlgorithmProvider(alg, 0); CloseHandle(file);
}
static void scan(const WCHAR *path, int maps) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    char line[8192], block[16384]; DWORD n; unsigned long total = 0; size_t used=0;
    if (file == INVALID_HANDLE_VALUE) { snprintf(line, sizeof line, "scan error=%lu\n", GetLastError()); emit(line); return; }
#ifdef WINE_EXIT_PROBE
    if (!maps) {
        LARGE_INTEGER size, start;
        if (!GetFileSizeEx(file, &size)) { CloseHandle(file); return; }
        start.QuadPart = size.QuadPart > 262144 ? size.QuadPart - 262144 : 0;
        if (!SetFilePointerEx(file, start, NULL, FILE_BEGIN)) { CloseHandle(file); return; }
    }
#endif
    while (total < 32 * 1024 * 1024 && ReadFile(file, block, sizeof block, &n, NULL) && n) {
      total += n;
      for (DWORD i=0; i<n; ++i) {
        line[used] = block[i];
        if (line[used] == '\n' || used == sizeof(line)-2) {
            line[++used] = 0;
            int keep = maps ? (strstr(line, "libarm64ecfex") || strstr(line, "libwow64fex")) :
                (strstr(line, "starting FEX") || strstr(line, "Loaded FEXUnixLib") || strstr(line, "=== time=") ||
                 strstr(line, "Unknown information class") || strstr(line, "[early-fault] sig=") ||
                 strstr(line, "libarm64ecfex.so") || strstr(line, "libwow64fex.so"));
#ifdef WINE_EXIT_PROBE
            if (keep || !maps) emit(line);
#else
            if (keep) emit(line);
#endif
            used=0;
        } else ++used;
      }
    }
    CloseHandle(file);
}
int main(void) {
#ifdef _WIN64
    const char *arch = "x64";
    const WCHAR *dest = L"\\\\?\\unix\\storage\\Users\\currentUser\\Download\\com.vintage.pomelopro\\" PROBE_LOG L"x64.log";
#else
    const char *arch = "x86";
    const WCHAR *dest = L"\\\\?\\unix\\storage\\Users\\currentUser\\Download\\com.vintage.pomelopro\\" PROBE_LOG L"x86.log";
#endif
    output=CreateFileW(dest, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (output == INVALID_HANDLE_VALUE) return 2;
    emit("fixed-readonly-fex-component-probe v1 arch="); emit(arch); emit("\n");
#ifdef _WIN64
    typedef LONG (WINAPI *query_t)(HANDLE, PVOID, int, PVOID, SIZE_T, SIZE_T *);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    query_t query = (query_t)GetProcAddress(ntdll, "NtQueryVirtualMemory");
    const WCHAR *names[] = {L"libarm64ecfex.dll", L"libwow64fex.dll"};
    for (unsigned i=0; i<2; ++i) {
        HMODULE module=GetModuleHandleW(names[i]);
        unsigned long long funcs=0;
        LONG status = module && query ? query((HANDLE)-1, module, 1000, &funcs, sizeof funcs, NULL) : -1;
        char result[256];
        snprintf(result, sizeof result, "unix-query module=%u present=%d status=%lx funcsPresent=%d dispatcherPresent=%d\n",
                 i, module != NULL, (unsigned long)status, funcs != 0,
                 GetProcAddress(ntdll, "__wine_unix_call_dispatcher") != NULL);
        emit(result);
    }
#endif
    hash_file("runtime-ec", L"\\\\?\\unix\\data\\storage\\el2\\base\\files\\wine\\bin\\aarch64-windows\\libarm64ecfex.dll");
    hash_file("runtime-wow", L"\\\\?\\unix\\data\\storage\\el2\\base\\files\\wine\\bin\\aarch64-windows\\libwow64fex.dll");
    hash_file("prefix-ec", L"\\\\?\\unix\\data\\storage\\el2\\base\\files\\.wine\\drive_c\\windows\\system32\\libarm64ecfex.dll");
    hash_file("prefix-wow", L"\\\\?\\unix\\data\\storage\\el2\\base\\files\\.wine\\drive_c\\windows\\system32\\libwow64fex.dll");
    hash_file("bridge-ec", L"\\\\?\\unix\\data\\storage\\el1\\bundle\\libs\\arm64\\libarm64ecfex.so");
    hash_file("bridge-wow", L"\\\\?\\unix\\data\\storage\\el1\\bundle\\libs\\arm64\\libwow64fex.so");
#ifdef WINE_COMPONENT_PROBE
    typedef const char *(__cdecl *version_t)(void);
    version_t version=(version_t)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"wine_get_version");
    if (version) { emit("wine-version="); emit(version()); emit("\n"); }
    hash_file("unix-ntdll", L"\\\\?\\unix\\data\\storage\\el1\\bundle\\libs\\arm64\\ntdll.so");
    hash_file("unix-win32u", L"\\\\?\\unix\\data\\storage\\el1\\bundle\\libs\\arm64\\win32u.so");
    hash_file("unix-vulkan", L"\\\\?\\unix\\data\\storage\\el1\\bundle\\libs\\arm64\\winevulkan.so");
    hash_file("unix-server", L"\\\\?\\unix\\data\\storage\\el1\\bundle\\libs\\arm64\\libwineserver.so");
#ifdef _WIN64
    hash_file("runtime-ntdll", L"\\\\?\\unix\\data\\storage\\el2\\base\\files\\wine\\bin\\aarch64-windows\\ntdll.dll");
    hash_file("prefix-ntdll", L"\\\\?\\unix\\data\\storage\\el2\\base\\files\\.wine\\drive_c\\windows\\system32\\ntdll.dll");
#else
    hash_file("runtime-ntdll", L"\\\\?\\unix\\data\\storage\\el2\\base\\files\\wine\\bin\\i386-windows\\ntdll.dll");
    hash_file("prefix-ntdll", L"\\\\?\\unix\\data\\storage\\el2\\base\\files\\.wine\\drive_c\\windows\\syswow64\\ntdll.dll");
#endif
#endif
#ifdef WINE_EXIT_PROBE
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W item = {.dwSize = sizeof(item)};
    if (snapshot != INVALID_HANDLE_VALUE && Process32FirstW(snapshot, &item)) do {
        const char *name = !_wcsicmp(item.szExeFile, L"winedbg.exe") ? "winedbg.exe" :
            !_wcsicmp(item.szExeFile, L"steam.exe") ? "steam.exe" :
            !_wcsicmp(item.szExeFile, L"steamwebhelper.exe") ? "steamwebhelper.exe" : NULL;
        if (name) {
            char line[256];
            snprintf(line, sizeof line, "process name=%s winpid=%lu parent=%lu\n",
                     name, item.th32ProcessID, item.th32ParentProcessID);
            emit(line);
        }
    } while (Process32NextW(snapshot, &item));
    if (snapshot != INVALID_HANDLE_VALUE) CloseHandle(snapshot);
#endif
    emit("self-maps\n"); scan(L"\\\\?\\unix\\proc\\self\\maps", 1);
#ifdef WINE_EXIT_PROBE
    emit("stderr-tail (last 256 KiB; historical timestamps)\n");
#else
    emit("startup-markers (historical log; use time headers)\n");
#endif
    SYSTEMTIME now; WCHAR logpath[256]; GetLocalTime(&now);
    swprintf(logpath, 256, L"\\\\?\\unix\\data\\storage\\el2\\base\\files\\wine-logs\\wine_stderr_%04u%02u%02u.log",
             now.wYear, now.wMonth, now.wDay);
    scan(logpath, 0);
    emit("probe-completed\n"); CloseHandle(output); return 0;
}
