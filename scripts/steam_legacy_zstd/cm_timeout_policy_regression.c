#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int vp_test_cm_install(unsigned char *client);
static unsigned failures;
static void check(const char *name,int ok) {
    printf("%s\t%s\n",name,ok?"PASS":"FAIL");failures+=!ok;
}
__attribute__((naked)) static unsigned invoke(void *thunk __attribute__((unused)),void *cm __attribute__((unused)),void *request __attribute__((unused))) {
    __asm__ volatile("push %ebx\n push %edi\n mov 12(%esp),%eax\n mov 16(%esp),%ebx\n"
                     "mov 20(%esp),%edi\n push $0\n push %edi\n call *%eax\n pop %edi\n pop %ebx\n ret\n");
}
static void fixture(unsigned char *image) {
    const uint32_t sites[]={0x3f21a9,0x3f2895};
    const unsigned char ctor[]={0xc7,0x87,0xac,0,0,0,0x60,0xea,0,0};
    memcpy(image+0xab058c,ctor,sizeof(ctor));
    for (unsigned i=0;i<2;++i) {
        int32_t rel=(int32_t)(0xab35c0-sites[i]-5);
        image[sites[i]]=0xe8;memcpy(image+sites[i]+1,&rel,4);
    }
    /* Return the request deadline, using the real enqueue function's ret 8
     * stack contract, so the generated thunk is executed, not just compared. */
    const unsigned char enqueue[]={0x8b,0x44,0x24,0x04,0x8b,0x80,0xac,0,0,0,0xc2,0x08,0};
    memcpy(image+0xab35c0,enqueue,sizeof(enqueue));
}
int main(void) {
    unsigned char *image=VirtualAlloc(NULL,0xc00000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    unsigned char cm[0x400]={0},request[0x200],original[sizeof(request)];
    if (!image) return 90;
    fixture(image);image[0xab058c]^=1;
    check("unknown-request-layout-keeps-calls",!vp_test_cm_install(image) && image[0x3f21aa]==0x12);
    fixture(image);image[0x3f2895]=0x90;
    check("unknown-second-call-keeps-first-call",!vp_test_cm_install(image) && image[0x3f21aa]==0x12);
    fixture(image);SetEnvironmentVariableA("VPP_STEAM_CM_TIMEOUT_DISABLE","1");
    check("diagnostic-opt-out-keeps-original",!vp_test_cm_install(image) && image[0x3f21aa]==0x12);
    SetEnvironmentVariableA("VPP_STEAM_CM_TIMEOUT_DISABLE",NULL);
    DWORD old;VirtualProtect(image,0xc00000,PAGE_EXECUTE_READ,&old);
    check("known-layout-installs",vp_test_cm_install(image));
    MEMORY_BASIC_INFORMATION page;
    VirtualQuery(image+0x3f21a9,&page,sizeof(page));
    check("call-page-protection-restored",page.Protect==PAGE_EXECUTE_READ);
    const uint32_t sites[]={0x3f21a9,0x3f2895};
    for (unsigned i=0;i<2;++i) {
        int32_t rel;memcpy(&rel,image+sites[i]+1,4);
        void *thunk=image+sites[i]+5+rel;
        VirtualQuery(thunk,&page,sizeof(page));
        check("thunk-is-execute-read",page.Protect==PAGE_EXECUTE_READ);
        const int counts[]={0,-1,1,32};
        for (unsigned j=0;j<4;++j) {
            memset(request,0x5a,sizeof(request));*(DWORD*)(request+0xac)=60000;
            memcpy(original,request,sizeof(request));
            *(int*)(cm+0x308)=counts[j];
            unsigned expected=counts[j]>0?5000:60000;
            check("cache-gate-and-stdcall-return",invoke(thunk,cm,request)==expected);
            *(DWORD*)(original+0xac)=expected;
            check("only-selected-request-timeout-changed",!memcmp(original,request,sizeof(request)));
        }
    }
    VirtualFree(image,0,MEM_RELEASE);
    printf("RESULT\tfailures=%u\n",failures);return failures?1:0;
}
