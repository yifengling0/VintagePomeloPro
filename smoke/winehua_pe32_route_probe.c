/* Same PE32 binary for CPU/backend comparisons. No graphics or game launch.
 * Run with the game closed. Compare checksums and x87 precision before times.
 * HODLL is read for attribution only; this program never changes the backend. */
#include <windows.h>
#include <emmintrin.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile uint32_t sink;
static unsigned char source[1024 * 1024], destination[1024 * 1024];
static const double factor = 0.9999, addition = 0.0001;
static LARGE_INTEGER frequency;

static uint32_t integer_probe(unsigned n)
{
    uint32_t x = 1, y = 0x9e3779b9;
    for (unsigned i = 0; i < n; ++i)
    {
        x = x * 1664525u + 1013904223u;
        y ^= x >> 7;
        y = (y << 5) | (y >> 27);
        if (x & 1) y += x;
    }
    return x ^ y;
}

static __attribute__((noinline)) uint32_t nested_calls(unsigned depth, uint32_t x)
{
    x = x * 1664525u + 1013904223u;
    if (!depth) return x;
    return nested_calls(depth - 1, x) ^ (x >> (depth & 7));
}
static uint32_t calls_probe(unsigned n)
{
    uint32_t x = 1;
    for (unsigned i = 0; i < n; ++i) x ^= nested_calls(16, x + i);
    return x;
}

static uint32_t copy_probe(unsigned n)
{
    for (unsigned i = 0; i < n; ++i)
    {
        const unsigned char *src = source;
        unsigned char *dst = destination;
        size_t words = sizeof(source) / 4;
        __asm__ volatile("cld; rep movsl" : "+S"(src), "+D"(dst), "+c"(words) : : "memory", "cc");
    }
    return memcmp(source, destination, sizeof(source)) ? 0 : 0x5a5a5a5a;
}

static uint32_t sse_probe(unsigned n)
{
    __m128 x = _mm_set_ps(1, 2, 3, 4);
    const __m128 mul = _mm_set1_ps(0.9999f), add = _mm_set1_ps(0.0001f);
    union { float f[4]; uint32_t u[4]; } result;
    for (unsigned i = 0; i < n; ++i) x = _mm_add_ps(_mm_mul_ps(x, mul), add);
    _mm_storeu_ps(result.f, x);
    uint32_t hash = 2166136261u;
    for (unsigned i = 0; i < 4; ++i) hash = (hash ^ result.u[i]) * 16777619u;
    return hash;
}

static uint32_t x87_probe(unsigned n)
{
    union { double d; uint32_t u[2]; } x;
    x.d = 3;
    for (unsigned i = 0; i < n; ++i)
        __asm__ volatile("fldl %0; fmull %1; faddl %2; fstpl %0"
            : "+m"(x.d) : "m"(factor), "m"(addition) : "st", "memory");
    return x.u[0] ^ x.u[1];
}

static uint32_t qpc_probe(unsigned n)
{
    LARGE_INTEGER before, now;
    if (!QueryPerformanceCounter(&before)) return 0;
    for (unsigned i = 0; i < n; ++i)
        if (!QueryPerformanceCounter(&now) || now.QuadPart < before.QuadPart) return 0;
    return 1;
}
static uint32_t yield_probe(unsigned n)
{
    for (unsigned i = 0; i < n; ++i) Sleep(0);
    return n;
}

static double precision_check(void)
{
    const double big = 9007199254740992.0, one = 1;
    double result;
    __asm__ volatile("fldl %1; faddl %2; fsubl %1; fstpl %0"
        : "=m"(result) : "m"(big), "m"(one) : "st");
    return result;
}

static int measure(FILE *out, const char *name, uint32_t (*probe)(unsigned), unsigned n)
{
    LARGE_INTEGER start, end;
    uint32_t expected = 0;
    int valid = 1;
    for (unsigned run = 0; run < 3; ++run)
    {
        if (!QueryPerformanceCounter(&start)) return 0;
        uint32_t value = probe(n);
        if (!QueryPerformanceCounter(&end) || end.QuadPart < start.QuadPart) return 0;
        sink ^= value;
        if (!run) expected = value;
        else if (value != expected) valid = 0;
        double ms = (double)(end.QuadPart - start.QuadPart) * 1000 / frequency.QuadPart;
        fprintf(out, "%s\t%u\t%u\t%.6f\t%08lx\t%d\n", name, run, n, ms,
            (unsigned long)value, valid);
        fflush(out);
    }
    return valid;
}

int main(void)
{
    char path[MAX_PATH], backend[MAX_PATH] = "";
    unsigned short saved_cw, extended_cw;
    int valid = 1;
    snprintf(path, sizeof(path), "C:\\vp32-route-probe-%lu.tsv", (unsigned long)GetCurrentProcessId());
    FILE *out = fopen(path, "wb");
    if (!out || !QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) return 2;
    GetEnvironmentVariableA("HODLL", backend, sizeof(backend));
    __asm__ volatile("fnstcw %0" : "=m"(saved_cw));
    extended_cw = (saved_cw & ~0x300u) | 0x300u;
    __asm__ volatile("fldcw %0" : : "m"(extended_cw));
    double precision = precision_check();
    valid &= precision == 1.0;
    fprintf(out, "probe=pe32-route-v1 pid=%lu pointerBits=%u HODLL=%s frequency=%lld\n",
        (unsigned long)GetCurrentProcessId(), (unsigned)(sizeof(void *) * 8), backend,
        (long long)frequency.QuadPart);
    fprintf(out, "x87_saved_cw=%04x x87_test_cw=%04x extended_precision_check=%.17g\n",
        saved_cw, extended_cw, precision);
    fprintf(out, "case\trun\titerations\twall_ms\tchecksum\tstable_within_run\n");
    for (size_t i = 0; i < sizeof(source); ++i) source[i] = (unsigned char)(i * 31 + 7);
    valid &= measure(out, "integer", integer_probe, 5000000);
    valid &= measure(out, "callret_depth16", calls_probe, 100000);
    valid &= measure(out, "rep_copy_1MiB", copy_probe, 256);
    valid &= measure(out, "sse_mul_add", sse_probe, 2000000);
    valid &= measure(out, "x87_mul_add", x87_probe, 200000);
    valid &= measure(out, "qpc_boundary", qpc_probe, 100000);
    valid &= measure(out, "sleep0_boundary", yield_probe, 10000);
    __asm__ volatile("fldcw %0" : : "m"(saved_cw));
    fprintf(out, "complete=1 precision_and_repeat_checks=%d sink=%08lx\n", valid, (unsigned long)sink);
    fclose(out);
    return valid ? 0 : 3;
}
