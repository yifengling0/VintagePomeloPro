/* Candidate: keep the signed steamclient.dll on disk unchanged. Install the
 * VSZa extension only in the legacy Steam UI process, after its original
 * verifier succeeds. Games load the original client without this extension.
 * This helper is called by the existing three SteamUI loader call sites;
 * no work runs in DllMain and no account/content verification is replaced. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>
#include <stdio.h>

typedef int (__cdecl *verifier_fn)(const char *);
static const unsigned char client_sha[32] = {
    0x95,0xa7,0xe0,0x1a,0x64,0xe2,0x0f,0x07,0x26,0xb6,0x3c,0xfb,0x63,0xb5,0x24,0x0e,
    0x51,0xc7,0xb1,0x59,0x40,0xb5,0x2d,0x03,0xe8,0xda,0x4c,0x41,0x2c,0xa5,0x46,0x69
};
static const unsigned char decoder_sha[32] = {
    0x57,0xc1,0xb0,0x50,0x3d,0x13,0x18,0x53,0xaa,0x89,0x9a,0x89,0x6d,0x3a,0x00,0x77,
    0xb9,0xf6,0x2a,0x44,0xec,0x55,0x45,0xdd,0x6c,0xe5,0x8e,0x0b,0x79,0x8e,0x0f,0xaf
};
static const unsigned char stolen[] = {0x55,0x8b,0xec,0x81,0xec,0x8c,0x02,0,0};
static LONG installed;
typedef int (__cdecl *decode_fn)(const unsigned char *, unsigned, void *, unsigned);
static decode_fn diagnostic_decode;
static wchar_t diagnostic_path[MAX_PATH];
static LONG diagnostic_count;

/* Opt-in, bounded evidence from the real downloader. The release decoder and
 * all its checks are unchanged. No contents, account data or URLs are logged. */
static int __cdecl decode_with_evidence(const unsigned char *data, unsigned size,
                                      void *buffer, unsigned max_output)
{
    int result = diagnostic_decode(data, size, buffer, max_output);
    LONG sequence = InterlockedIncrement(&diagnostic_count);
    if (sequence <= 512) {
        HANDLE file = CreateFileW(diagnostic_path, FILE_APPEND_DATA,
            FILE_SHARE_READ|FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file != INVALID_HANDLE_VALUE) {
            SYSTEMTIME time; GetSystemTime(&time);
            char line[224]; DWORD written;
            int n = snprintf(line, sizeof(line),
                "%04u-%02u-%02uT%02u:%02u:%02uZ pid=%lu tid=%lu bits=32 seq=%ld format=VSZa input=%u result=%d\r\n",
                time.wYear,time.wMonth,time.wDay,time.wHour,time.wMinute,time.wSecond,
                GetCurrentProcessId(),GetCurrentThreadId(),sequence,size,result);
            if (n > 0 && n < (int)sizeof(line)) WriteFile(file,line,(DWORD)n,&written,NULL);
            CloseHandle(file);
        }
    }
    return result;
}

static FARPROC select_decode(FARPROC decode, const wchar_t *client_path)
{
    char flag[4];
    if (GetEnvironmentVariableA("VPP_STEAM_RUNTIME_DIAG", flag, sizeof(flag)) != 1 || flag[0] != '1')
        return decode;
    wcscpy(diagnostic_path, client_path);
    wchar_t *tail = wcsrchr(diagnostic_path, L'\\');
    if (!tail || tail - diagnostic_path + 32 >= MAX_PATH) return decode;
    wcscpy(tail + 1, L"logs\\vpsteam-runtime-decode.log");
    diagnostic_decode = (decode_fn)decode;
    return (FARPROC)decode_with_evidence;
}

static int hash_matches(const wchar_t *path, const unsigned char expected[32])
{
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                             OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    LARGE_INTEGER size;
    unsigned char buffer[32768], digest[32];
    DWORD got;
    int ok = 0;
    if (file == INVALID_HANDLE_VALUE) return 0;
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > (64LL<<20)) goto done;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0) goto done;
    if (BCryptCreateHash(alg, &hash, NULL, 0, NULL, 0, 0) < 0) goto done;
    for (;;) {
        if (!ReadFile(file, buffer, sizeof(buffer), &got, NULL)) goto done;
        if (!got) break;
        if (BCryptHashData(hash, buffer, got, 0) < 0) goto done;
    }
    if (BCryptFinishHash(hash, digest, sizeof(digest), 0) < 0) goto done;
    ok = !memcmp(digest, expected, sizeof(digest));
done:
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(file);
    return ok;
}

static int guarded_paths(const char *path, wchar_t *client, wchar_t *decoder)
{
    wchar_t input[MAX_PATH], full[MAX_PATH], ui_path[MAX_PATH], exe[MAX_PATH];
    DWORD n;
    HMODULE ui = GetModuleHandleW(L"SteamUI.dll");
    if (!path || !*path || !ui ||
        !MultiByteToWideChar(CP_ACP, 0, path, -1, input, MAX_PATH)) return 0;
    n = GetFullPathNameW(input, MAX_PATH, full, NULL);
    if (!n || n >= MAX_PATH) return 0;
    n = GetModuleFileNameW(ui, ui_path, MAX_PATH);
    if (!n || n >= MAX_PATH) return 0;
    wchar_t *tail = wcsrchr(ui_path, L'\\');
    if (!tail) return 0;
    tail[1] = 0;
    if (wcslen(ui_path) + 24 >= MAX_PATH) return 0;
    wcscpy(client, ui_path); wcscat(client, L"steamclient.dll");
    wcscpy(decoder, ui_path); wcscat(decoder, L"vpsteamzstd32.dll");
    if (_wcsicmp(full, client)) return 0;
    n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (!n || n >= MAX_PATH) return 0;
    wcscat(ui_path, L"steam.exe");
    /* Same directory and process: never install in a game's Steam API load. */
    return !_wcsicmp(exe, ui_path) && hash_matches(client, client_sha) &&
        hash_matches(decoder, decoder_sha);
}

static void rel32(unsigned char *operand, const void *target)
{
    uint32_t delta = (uint32_t)(uintptr_t)target - (uint32_t)(uintptr_t)(operand + 4);
    memcpy(operand, &delta, 4);
}

static int install_decoder(const wchar_t *client_path, const wchar_t *decoder_path)
{
    HMODULE client = LoadLibraryExW(client_path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    HMODULE decoder = NULL, pin;
    unsigned char *code = NULL, *entry;
    FARPROC decode;
    DWORD old, ignored;
    int ok = 0;
    if (!client) return 0;
    entry = (unsigned char *)client + 0xa1cb00;
    if (memcmp(entry, stolen, sizeof(stolen))) goto done;
    decoder = LoadLibraryExW(decoder_path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!decoder || !(decode = GetProcAddress(decoder, "VPDecodeVSZa"))) goto done;
    decode = select_decode(decode, client_path);
    code = VirtualAlloc(NULL, 4096, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
    if (!code) goto done;
    /* cdecl: inspect size and format without changing the stack. All original
     * formats replay the complete stolen prologue. VSZa tail-calls the helper. */
    static const unsigned char dispatch[] = {
        0x8b,0x44,0x24,0x04, 0x83,0x7c,0x24,0x08,0x04,
        0x0f,0x82,0,0,0,0, 0x81,0x38,0x56,0x53,0x5a,0x61,
        0x0f,0x85,0,0,0,0, 0xb8,0,0,0,0,0xff,0xe0
    };
    memcpy(code, dispatch, sizeof(dispatch));
    rel32(code + 11, code + sizeof(dispatch));
    rel32(code + 23, code + sizeof(dispatch));
    uint32_t function = (uint32_t)(uintptr_t)decode;
    memcpy(code + 28, &function, 4);
    memcpy(code + sizeof(dispatch), stolen, sizeof(stolen));
    code[sizeof(dispatch) + sizeof(stolen)] = 0xe9;
    rel32(code + sizeof(dispatch) + sizeof(stolen) + 1, entry + sizeof(stolen));
    if (!VirtualProtect(code, 4096, PAGE_EXECUTE_READ, &old) ||
        !FlushInstructionCache(GetCurrentProcess(), code, 64)) goto done;
    /* Called during the single-threaded UI client load, before any downloader
     * has this decoder address. Do not apply this mechanism to a live worker. */
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
                           (LPCWSTR)client, &pin) ||
        !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
                           (LPCWSTR)decoder, &pin) ||
        !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
                           (LPCWSTR)decode_with_evidence, &pin)) goto done;
    if (!VirtualProtect(entry, sizeof(stolen), PAGE_EXECUTE_READWRITE, &old)) goto done;
    unsigned char branch[sizeof(stolen)];
    memset(branch, 0x90, sizeof(branch)); branch[0] = 0xe9;
    uint32_t displacement = (uint32_t)(uintptr_t)code - (uint32_t)(uintptr_t)(entry + 5);
    memcpy(branch + 1, &displacement, 4);
    memcpy(entry, branch, sizeof(branch));
    if (!FlushInstructionCache(GetCurrentProcess(), entry, sizeof(branch))) {
        memcpy(entry, stolen, sizeof(stolen));
        FlushInstructionCache(GetCurrentProcess(), entry, sizeof(stolen));
        VirtualProtect(entry, sizeof(stolen), old, &ignored);
        goto done;
    }
    if (!VirtualProtect(entry, sizeof(stolen), old, &ignored)) {
        memcpy(entry, stolen, sizeof(stolen));
        FlushInstructionCache(GetCurrentProcess(), entry, sizeof(stolen));
        VirtualProtect(entry, sizeof(stolen), old, &ignored);
        goto done;
    }
    ok = 1;
done:
    if (!ok && code) VirtualFree(code, 0, MEM_RELEASE);
    if (decoder) FreeLibrary(decoder); /* pinned on success */
    FreeLibrary(client);
    return ok;
}

static void status_log(const wchar_t *client, int ok)
{
    wchar_t path[MAX_PATH];
    wcscpy(path, client);
    wchar_t *tail = wcsrchr(path, L'\\');
    if (!tail || tail - path + 28 >= MAX_PATH) return;
    wcscpy(tail + 1, L"logs\\vpsteam-runtime32.log");
    HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ|FILE_SHARE_WRITE,
                              NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME time; GetSystemTime(&time);
    char line[192]; DWORD written;
    int n = snprintf(line, sizeof(line),
        "%04u-%02u-%02uT%02u:%02u:%02uZ pid=%lu install=%d original-client-on-disk=1\r\n",
        time.wYear,time.wMonth,time.wDay,time.wHour,time.wMinute,time.wSecond,
        GetCurrentProcessId(),ok);
    if (n > 0 && n < (int)sizeof(line)) WriteFile(file,line,(DWORD)n,&written,NULL);
    CloseHandle(file);
}

__declspec(dllexport) int __cdecl VPValidateLegacyClient(const char *path, verifier_fn original)
{
    /* The original signature verifier always makes the trust decision. */
    int result = original ? original(path) : 5;
    wchar_t client[MAX_PATH], decoder[MAX_PATH];
    if (result || !guarded_paths(path, client, decoder)) return result;
    if (InterlockedCompareExchange(&installed, 1, 0) == 0) {
        /* A late caller may already have downloader threads using the entry.
         * Fail closed for that case; only extend the first client load. */
        int ok = !GetModuleHandleW(L"steamclient.dll") && install_decoder(client, decoder);
        InterlockedExchange(&installed, ok ? 2 : -1);
        status_log(client, ok);
        OutputDebugStringA(ok ? "[vpsteam] runtime32 decoder installed; original client on disk\n"
                             : "[vpsteam] runtime32 decoder unavailable; original decoder retained\n");
    }
    return result;
}

#ifdef VP_RUNTIME_REGRESSION
/* Only in the test executable, never exported by the candidate DLL. */
int vp_test_install(const wchar_t *client, const wchar_t *decoder)
{
    return hash_matches(client, client_sha) && hash_matches(decoder, decoder_sha) &&
        install_decoder(client, decoder);
}
#endif
