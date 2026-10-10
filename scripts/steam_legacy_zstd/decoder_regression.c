#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <stdlib.h>

/* FIXTURES */
typedef struct {
    uintptr_t element_size; unsigned char *memory;
    int32_t allocation_count,grow_size,get,put,max_put;
    uint16_t tab; uint8_t error,flags; void *overflow[2];
} buffer_t;
#ifdef _WIN64
#define DLL "steamclient64.dll"
#define CTOR 0xb82f50
#define DTOR 0xb88580
#define DECODER 0xb9a1d0
#define SEEK 0xb85990
#define UNPACK 0x3fcea0
#define BASE 0x138000000ull
#define CALL
#else
#define DLL "steamclient.dll"
#define CTOR 0xa09410
#define DTOR 0xa0dd60
#define DECODER 0xa1cb00
#define SEEK 0xa0b880
#define UNPACK 0x363b90
#define BASE 0x38000000ull
#define CALL __attribute__((thiscall))
#endif
typedef void (CALL *ctor_t)(void *,int,int,int);
typedef void (CALL *dtor_t)(void *);
typedef void (CALL *seek_t)(void *,int,int);
typedef int (*decode_t)(const void *,unsigned,void *,unsigned);
typedef unsigned (*unpack_t)(void *,unsigned,unsigned,void *,void *);
static FILE *out;
static ctor_t ctor; static dtor_t dtor; static seek_t seek;
static decode_t decode; static unpack_t unpack;
static LONG failures,checks;

static void check(const char *name,int pass) {
    InterlockedIncrement(&checks);
    if (!pass) InterlockedIncrement(&failures);
    fprintf(out,"%s\t%s\n",name,pass?"PASS":"FAIL"); fflush(out);
}
static int digest(const void *data,unsigned len,LPCWSTR algorithm,void *hash,unsigned size) {
    BCRYPT_ALG_HANDLE alg=NULL; int ok=0;
    if (!BCryptOpenAlgorithmProvider(&alg,algorithm,NULL,0)) {
        ok=!BCryptHash(alg,NULL,0,(PUCHAR)data,len,hash,size);
        BCryptCloseAlgorithmProvider(alg,0);
    }
    return ok;
}
static int expected(const unsigned char *data,unsigned n,const unsigned char sha[20]) {
    unsigned char hash[20];
    return data && digest(data,n,BCRYPT_SHA1_ALGORITHM,hash,20) && !memcmp(hash,sha,20);
}
static int decoded(const unsigned char *data,unsigned n,unsigned size,const unsigned char sha[20],int append) {
    buffer_t b={0}; ctor(&b,0,append?3:0,0);
    if (append) { memcpy(b.memory,"abc",3);seek(&b,1,3); }
    int r=decode(data,n,&b,128u<<20);
    int ok=r==1 && b.put==(int)size+(append?3:0) && b.max_put==b.put &&
        (!append || !memcmp(b.memory,"abc",3)) && expected(b.memory+(append?3:0),size,sha);
    dtor(&b); return ok;
}
static int unpacked(const unsigned char *data,unsigned n,const unsigned char key[32],unsigned size,const unsigned char sha[20]) {
    buffer_t kb={0},input={0};ctor(&kb,0,32,0);ctor(&input,0,n,0);
    memcpy(kb.memory,key,32); seek(&kb,1,32);memcpy(input.memory,data,n);
    unsigned written=unpack(&kb,0,n,&input,NULL);
    int ok=written==size && expected(input.memory,size,sha);
    fprintf(out,"encrypted-unpack\tbytes=%u\toutput=%u\texpected=%u\t%s\n",n,written,size,ok?"PASS":"FAIL");
    dtor(&kb); dtor(&input); return ok;
}
static void invalid(const char *name,const unsigned char *data,unsigned n,unsigned cap,int want) {
    buffer_t b={0};ctor(&b,0,0,0);
    int r=decode(data,n,&b,cap);
    check(name,r==want && b.put==0 && b.max_put==0 && !b.memory);
    dtor(&b);
}
static DWORD WINAPI concurrent(void *unused) {
    (void)unused;
    for (int i=0;i<40;++i) {
        int ok=decoded(zstd_data,sizeof(zstd_data),ZSTD_OUTPUT,zstd_sha1,0) &&
            decoded(vzip_data,sizeof(vzip_data),VZIP_OUTPUT,vzip_sha1,0);
        InterlockedIncrement(&checks);
        if (!ok) InterlockedIncrement(&failures);
    }
    return 0;
}

int main(int argc,char **argv) {
    (void)argv;
    char path[MAX_PATH]; if (!GetModuleFileNameA(NULL,path,sizeof(path))) return 90;
    char *slash=strrchr(path,'\\');if (!slash) return 90;*slash=0;
    if (!SetCurrentDirectoryA(path)) return 90;
    out=fopen("decoder-regression-result.tsv","w");if (!out) return 90;
    SYSTEMTIME time;GetSystemTime(&time);
    fprintf(out,"regression\tbits=%u\tpid=%lu\tutc=%04u-%02u-%02uT%02u:%02u:%02u.%03uZ\n",(unsigned)(8*sizeof(void *)),(unsigned long)GetCurrentProcessId(),time.wYear,time.wMonth,time.wDay,time.wHour,time.wMinute,time.wSecond,time.wMilliseconds);fflush(out);
    FILE *f=fopen(DLL,"rb"); if (!f) return 91;
    fseek(f,0,SEEK_END);long n=ftell(f);fseek(f,0,SEEK_SET);
    unsigned char *file=malloc(n),sha256[32];
    if (!file || fread(file,1,n,f)!=(size_t)n) return 91;
    fclose(f);
    int match=digest(file,(unsigned)n,BCRYPT_SHA256_ALGORITHM,sha256,32) && !memcmp(sha256,dll_sha256,32);free(file);
    check("exact-dll-hash",match);if (!match) return 92;
    void *reserved=NULL;
    if (argc>1) reserved=VirtualAlloc((void *)(uintptr_t)BASE,reserve_size,MEM_RESERVE,PAGE_NOACCESS);
    HMODULE module=LoadLibraryA(DLL);
    if (!module) {fprintf(out,"load-error=%lu\n",(unsigned long)GetLastError());return 93;}
    fprintf(out,"loaded-base=%p\treserved-preferred=%p\n",module,reserved);fflush(out);
    if (argc>1) check("relocation",reserved && (uintptr_t)module!=BASE);
    const unsigned char *base=(const unsigned char *)module;
    ctor=(ctor_t)(base+CTOR);dtor=(dtor_t)(base+DTOR);seek=(seek_t)(base+SEEK);
    decode=(decode_t)(base+DECODER);unpack=(unpack_t)(base+UNPACK);
    check("zip-original-route",decoded(pkzip_data,sizeof(pkzip_data),PKZIP_OUTPUT,pkzip_sha1,0));
    check("lzma-original-route",decoded(vzip_data,sizeof(vzip_data),VZIP_OUTPUT,vzip_sha1,0));
    check("zstd-new-route",decoded(zstd_data,sizeof(zstd_data),ZSTD_OUTPUT,zstd_sha1,0));
    check("zstd-append-cursor",decoded(zstd_data,sizeof(zstd_data),ZSTD_OUTPUT,zstd_sha1,1));
    check("large-zstd-block",decoded(large_data,sizeof(large_data),LARGE_OUTPUT,large_sha1,0));
    check("large-encrypted-chain",unpacked(large_encrypted,sizeof(large_encrypted),large_key,LARGE_OUTPUT,large_sha1));
    invalid("empty-zstd",empty_data,sizeof(empty_data),128u<<20,1);
    /* ENCRYPTED_CALLS */
    unsigned char bad[sizeof(zstd_data)];memcpy(bad,zstd_data,sizeof(bad));
    bad[sizeof(bad)-1]^=1;invalid("bad-footer",bad,sizeof(bad),128u<<20,53);
    memcpy(bad,zstd_data,sizeof(bad));bad[4]^=1;invalid("header-footer-crc",bad,sizeof(bad),128u<<20,53);
    memcpy(bad,zstd_data,sizeof(bad));bad[4]^=1;bad[sizeof(bad)-15]^=1;invalid("bad-output-crc",bad,sizeof(bad),128u<<20,53);
    memcpy(bad,zstd_data,sizeof(bad));bad[8]=0;invalid("bad-zstd-frame",bad,sizeof(bad),128u<<20,53);
    invalid("truncated",zstd_data,23,128u<<20,2);
    invalid("declared-limit",zstd_data,sizeof(zstd_data),ZSTD_OUTPUT-1,25);
    memcpy(bad,zstd_data,sizeof(bad));memset(bad+sizeof(bad)-11,0xff,8);invalid("overflow-footer-size",bad,sizeof(bad),128u<<20,25);
    HANDLE threads[4];for (int i=0;i<4;++i) threads[i]=CreateThread(NULL,0,concurrent,NULL,0,NULL);
    int valid=threads[0] && threads[1] && threads[2] && threads[3];check("worker-create",valid);
    if (valid) {check("concurrent-completion",WaitForMultipleObjects(4,threads,TRUE,30000)==WAIT_OBJECT_0);for(int i=0;i<4;++i)CloseHandle(threads[i]);}
    if (GetEnvironmentVariableA("VPP_STEAM_DIAG_CAP_TEST",path,sizeof(path)))
        for (unsigned j=0;j<600;j++)
            check("logging-cap-does-not-change-output",decoded(zstd_data,sizeof(zstd_data),ZSTD_OUTPUT,zstd_sha1,0));
    fprintf(out,"RESULT\tchecks=%ld\tfailures=%ld\n",checks,failures);fclose(out);
    return failures?1:0;
}
