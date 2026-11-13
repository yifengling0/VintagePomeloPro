/* Read-only component probe. Fixed inputs, bounded reads, no game/account files. */
#include <windows.h>
#include <bcrypt.h>
#include <stdio.h>
#include <string.h>

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
            if (keep) emit(line);
            used=0;
        } else ++used;
      }
    }
    CloseHandle(file);
}
int main(void) {
#ifdef _WIN64
    const char *arch = "x64";
    const WCHAR *dest = L"\\\\?\\unix\\storage\\Users\\currentUser\\Download\\com.vintage.pomelopro\\fex-probe-x64.log";
#else
    const char *arch = "x86";
    const WCHAR *dest = L"\\\\?\\unix\\storage\\Users\\currentUser\\Download\\com.vintage.pomelopro\\fex-probe-x86.log";
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
    emit("self-maps\n"); scan(L"\\\\?\\unix\\proc\\self\\maps", 1);
    emit("startup-markers (historical log; use time headers)\n");
    SYSTEMTIME now; WCHAR logpath[256]; GetLocalTime(&now);
    swprintf(logpath, 256, L"\\\\?\\unix\\data\\storage\\el2\\base\\files\\wine-logs\\wine_stderr_%04u%02u%02u.log",
             now.wYear, now.wMonth, now.wDay);
    scan(logpath, 0);
    emit("probe-completed\n"); CloseHandle(output); return 0;
}
