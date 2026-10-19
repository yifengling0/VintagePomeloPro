#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <cpuid.h>

/* Check architectural results, including overwriting a prior shared zero,
 * all lanes, AVX upper halves, and a non-self XOR. No game or .NET dependency. */
static const float scalar = 800.0f;
#define SSE_CASE(name, op) \
__attribute__((noinline)) static void name(uint32_t *out) { \
    __asm__ volatile(op " %%xmm1,%%xmm1\n\t" \
      "movups %%xmm1,0(%0)\n\t" "movss %1,%%xmm1\n\t" \
      op " %%xmm3,%%xmm3\n\t" op " %%xmm0,%%xmm0\n\t" \
      "movups %%xmm0,32(%0)\n\t" "movups %%xmm1,64(%0)\n\t" \
      "movups %%xmm3,96(%0)\n\t" : : "r"(out),"m"(scalar) \
      : "xmm0","xmm1","xmm3","memory"); }
SSE_CASE(sse_xorps, "xorps")
SSE_CASE(sse_xorpd, "xorpd")
SSE_CASE(sse_pxor, "pxor")

#define AVX_CASE(name, op, reg) \
__attribute__((target("avx"),noinline)) static void name(uint32_t *out) { \
    __asm__ volatile(op " %%" reg "1,%%" reg "1,%%" reg "1\n\t" \
      "vmovups %%" reg "1,0(%0)\n\t" "vmovss %1,%%xmm1\n\t" \
      op " %%" reg "3,%%" reg "3,%%" reg "3\n\t" \
      op " %%" reg "0,%%" reg "0,%%" reg "0\n\t" \
      "vmovups %%" reg "0,32(%0)\n\t" "vmovups %%" reg "1,64(%0)\n\t" \
      "vmovups %%" reg "3,96(%0)\n\t" "vzeroupper" \
      : : "r"(out),"m"(scalar) : "xmm0","xmm1","xmm3","memory"); }
AVX_CASE(avx_xorps, "vxorps", "xmm")
AVX_CASE(avx_xorpd, "vxorpd", "xmm")
AVX_CASE(avx_pxor, "vpxor", "xmm")
AVX_CASE(avx256_xorps, "vxorps", "ymm")
AVX_CASE(avx256_xorpd, "vxorpd", "ymm")

__attribute__((noinline)) static void nonself(uint32_t *out) {
    const uint32_t a[4] = {0x11111111,0x22222222,0x33333333,0x44444444};
    const uint32_t b[4] = {0x12345678,0x87654321,0xffffffff,0};
    __asm__ volatile("movups %1,%%xmm0\n\t" "movups %2,%%xmm1\n\t"
                     "xorps %%xmm1,%%xmm0\n\t" "movups %%xmm0,%0"
                     : "=m"(*(uint32_t(*)[4])out) : "m"(a),"m"(b) : "xmm0","xmm1");
}
static int has_avx(void) {
    unsigned a,b,c,d;
    if (!__get_cpuid(1,&a,&b,&c,&d) || (c & (1u<<28|1u<<27)) != (1u<<28|1u<<27)) return 0;
    __asm__ volatile("xgetbv" : "=a"(a), "=d"(d) : "c"(0));
    return (a & 6) == 6;
}
int main(void) {
    struct {const char *name; void (*run)(uint32_t*); int lanes, avx;} tests[] = {
        {"xorps",sse_xorps,4,0},{"xorpd",sse_xorpd,4,0},{"pxor",sse_pxor,4,0},
        {"vxorps128",avx_xorps,4,1},{"vxorpd128",avx_xorpd,4,1},{"vpxor128",avx_pxor,4,1},
        {"vxorps256",avx256_xorps,8,1},{"vxorpd256",avx256_xorpd,8,1}};
    char log[4096]; int length=0, errors=0, avx=has_avx();
    for (unsigned t=0;t<sizeof(tests)/sizeof(tests[0]);++t) {
        if (tests[t].avx && !avx) {length+=sprintf(log+length,"SKIP %s AVX unavailable\n",tests[t].name);continue;}
        int failed=0;
        for (int repeat=0;repeat<64;++repeat) {
            uint32_t result[32]; memset(result,0xcd,sizeof(result));
            tests[t].run(result);
            for (int v=0;v<4;++v) for (int lane=0;lane<tests[t].lanes;++lane) {
                const uint32_t expected = v==2 && lane==0 ? 0x44480000 : 0;
                if(result[v*8+lane]!=expected) failed=1;
            }
        }
        errors+=failed; length+=sprintf(log+length,"%s %s 64 repeats\n",failed?"FAIL":"PASS",tests[t].name);
    }
    uint32_t other[4]; nonself(other);
    const uint32_t expected[4]={0x03254769,0xa5476103,0xcccccccc,0x44444444};
    if(memcmp(other,expected,sizeof(other))) ++errors;
    length+=sprintf(log+length,"nonself=%s errors=%d avx=%d\n",memcmp(other,expected,sizeof(other))?"FAIL":"PASS",errors,avx);
    HANDLE f=CreateFileW(L"C:\\vp-simd-zero-result.txt",GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    if(f==INVALID_HANDLE_VALUE)return 250;
    DWORD written=0;BOOL ok=WriteFile(f,log,length,&written,NULL);CloseHandle(f);
    return ok&&written==(DWORD)length?errors:251;
}
