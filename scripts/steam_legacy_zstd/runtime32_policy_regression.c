/* Run in an isolated copy of the original Steam DLLs plus vpsteamzstd32.dll.
 * Compile this executable with -DVP_RUNTIME_REGRESSION and vpsteam_runtime32.c.
 * It never starts Steam, contacts a server, or changes the input DLLs. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

typedef int (__cdecl *verifier_fn)(const char *);
int __cdecl VPValidateLegacyClient(const char *, verifier_fn);
int vp_test_install(const wchar_t *, const wchar_t *);
static int failures, calls;
static int __cdecl rejected(const char *path) { (void)path; ++calls; return 77; }
static int __cdecl accepted(const char *path) { (void)path; ++calls; return 0; }
static void check(const char *name, int ok) {
    printf("%s\t%s\n",name,ok?"PASS":"FAIL");
    failures += !ok;
}
static int corrupt_copy(const char *source, const char *target) {
    if (!CopyFileA(source,target,TRUE)) return 0;
    FILE *file=fopen(target,"r+b"); if(!file)return 0;
    int value=fgetc(file); rewind(file);
    int ok=value!=EOF && fputc(value^1,file)!=EOF && !fflush(file);
    fclose(file); return ok;
}
int main(void) {
    check("original-rejection-propagated",VPValidateLegacyClient("steamclient.dll",rejected)==77 && calls==1);
    check("missing-verifier-rejected",VPValidateLegacyClient("steamclient.dll",NULL)==5 && calls==1);
    check("non-steam-process-keeps-original",VPValidateLegacyClient("steamclient.dll",accepted)==0 && calls==2 && !GetModuleHandleW(L"steamclient.dll"));
    check("unknown-path-keeps-original-result",VPValidateLegacyClient("missing.dll",rejected)==77 && calls==3);
    if(!corrupt_copy("steamclient.dll","policy-corrupt-client.dll") ||
       !corrupt_copy("vpsteamzstd32.dll","policy-corrupt-decoder.dll"))return 90;
    check("tampered-client-rejected",!vp_test_install(L"policy-corrupt-client.dll",L"vpsteamzstd32.dll"));
    check("tampered-helper-rejected",!vp_test_install(L"steamclient.dll",L"policy-corrupt-decoder.dll"));
    DeleteFileA("policy-corrupt-client.dll"); DeleteFileA("policy-corrupt-decoder.dll");
    printf("RESULT\tfailures=%d\n",failures);
    return failures?1:0;
}
