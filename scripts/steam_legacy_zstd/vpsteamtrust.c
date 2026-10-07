/* Replace Valve's file-signature requirement only for the exact Zstd-patched
 * legacy client and decoder pair. All other inputs use the original verifier.
 * This has no effect on account, ownership, depot or download-content checks. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>

typedef int (__cdecl *verifier_fn)(const char *);
static const unsigned char client_sha[32] = {
    0x9c,0x45,0xde,0x0b,0x9a,0x5f,0xcc,0x67,0xe6,0xbb,0xfb,0x28,0xf5,0x42,0x88,0xda,
    0x0b,0x19,0xf6,0x03,0x32,0xa7,0x50,0x94,0x4d,0xfc,0x58,0xe6,0xae,0xbd,0xde,0xcd
};
static const unsigned char decoder_sha[32] = {
    0x57,0xc1,0xb0,0x50,0x3d,0x13,0x18,0x53,0xaa,0x89,0x9a,0x89,0x6d,0x3a,0x00,0x77,
    0xb9,0xf6,0x2a,0x44,0xec,0x55,0x45,0xdd,0x6c,0xe5,0x8e,0x0b,0x79,0x8e,0x0f,0xaf
};

static int hash_matches(const wchar_t *path, const unsigned char expected[32]) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                             OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER size;
    int ok = 0;
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    unsigned char buffer[32768], digest[32];
    DWORD got;
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

static int known_client(const char *path) {
    wchar_t requested[MAX_PATH], full[MAX_PATH], expected[MAX_PATH], decoder[MAX_PATH];
    if (!path || !*path) return 0;
    /* Match the legacy loader's normal ACP filename conversion. */
    if (!MultiByteToWideChar(CP_ACP, 0, path, -1, requested, MAX_PATH)) return 0;
    DWORD length = GetFullPathNameW(requested, MAX_PATH, full, NULL);
    if (!length || length >= MAX_PATH) return 0;
    HMODULE ui = GetModuleHandleW(L"SteamUI.dll");
    if (!ui) return 0;
    length = GetModuleFileNameW(ui, expected, MAX_PATH);
    if (!length || length >= MAX_PATH) return 0;
    wchar_t *tail = wcsrchr(expected, L'\\');
    if (!tail) return 0;
    tail[1] = 0;
    if (wcslen(expected) + 21 >= MAX_PATH) return 0;
    wcscpy(decoder, expected);
    wcscat(expected, L"steamclient.dll");
    if (_wcsicmp(full, expected)) return 0;
    wcscat(decoder, L"vpsteamzstd32.dll");
    return hash_matches(full, client_sha) && hash_matches(decoder, decoder_sha);
}

/* cdecl preserves the original file-verifier result convention: zero is OK. */
__declspec(dllexport) int __cdecl VPValidateLegacyClient(const char *path, verifier_fn original) {
    if (known_client(path)) return 0;
    return original ? original(path) : 5;
}
