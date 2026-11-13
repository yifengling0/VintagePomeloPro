#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <ctype.h>
#include <string.h>

typedef int (__cdecl *original_fn)(const char *);
typedef int (__cdecl *validate_fn)(const char *, original_fn);
static unsigned calls, failures, checks;
static int __cdecl original(const char *path) { (void)path; ++calls; return 73; }
static void check(const char *name, int value, int expected) {
    ++checks;
    printf("%s\t%d\t%d\t%s\n", name, value, expected, value==expected ? "PASS" : "FAIL");
    if (value!=expected) ++failures;
}
static int flip(const char *name) {
    HANDLE h = CreateFileA(name, GENERIC_READ|GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h==INVALID_HANDLE_VALUE) return 0;
    unsigned char b; DWORD got;
    int ok = ReadFile(h, &b, 1, &got, NULL) && got==1;
    if (ok) {
        b ^= 1;
        SetFilePointer(h, 0, NULL, FILE_BEGIN);
        ok = WriteFile(h, &b, 1, &got, NULL) && got==1;
    }
    CloseHandle(h);
    return ok;
}
int main(void) {
    HMODULE stub = LoadLibraryA("SteamUI.dll");
    HMODULE helper = LoadLibraryA("vpsteamtrust32.dll");
    if (!stub || !helper) { printf("load-error\t%lu\n", GetLastError()); return 99; }
    validate_fn validate = (validate_fn)(void *)GetProcAddress(helper, "VPValidateLegacyClient");
    if (!validate) return 98;
    char full[MAX_PATH], outside[MAX_PATH];
    GetFullPathNameA("steamclient.dll", MAX_PATH, full, NULL);
    unsigned before = calls;
    check("known-pair-relative", validate("steamclient.dll", original), 0);
    check("known-pair-absolute", validate(full, original), 0);
    for (char *c=full; *c; ++c) *c=(char)toupper((unsigned char)*c);
    check("known-pair-case", validate(full, original), 0);
    check("known-pair-no-fallback", calls-before, 0);
    check("null-name", validate(NULL, original), 73);
    check("empty-name", validate("", original), 73);
    check("other-client", validate("steamclient64.dll", original), 73);
    check("decoder-as-client", validate("vpsteamzstd32.dll", original), 73);
    CreateDirectoryA("other", NULL);
    CopyFileA("steamclient.dll", "other\\steamclient.dll", FALSE);
    GetFullPathNameA("other\\steamclient.dll", MAX_PATH, outside, NULL);
    check("same-hash-other-directory", validate(outside, original), 73);
    if (!flip("steamclient.dll")) return 97;
    check("corrupt-client", validate("steamclient.dll", original), 73);
    if (!flip("steamclient.dll")) return 96;
    if (!flip("vpsteamzstd32.dll")) return 95;
    check("corrupt-decoder", validate("steamclient.dll", original), 73);
    if (!flip("vpsteamzstd32.dll")) return 94;
    if (!MoveFileA("vpsteamzstd32.dll", "decoder-hidden.dll")) return 93;
    check("missing-decoder", validate("steamclient.dll", original), 73);
    if (!MoveFileA("decoder-hidden.dll", "vpsteamzstd32.dll")) return 92;
    check("pair-restored", validate("steamclient.dll", original), 0);
    check("null-verifier-fails", validate("other.dll", NULL), 5);
    printf("SUMMARY\t%u checks\t%u failures\n", checks, failures);
    return failures ? 1 : 0;
}
