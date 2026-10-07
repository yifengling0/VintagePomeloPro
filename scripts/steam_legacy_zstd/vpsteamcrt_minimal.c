/* Diagnostic alternative to MinGW DLL CRT startup. No TLS or FPU init. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stddef.h>

BOOL WINAPI VPHelperEntry(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)instance; (void)reason; (void)reserved;
    return TRUE;
}
void *malloc(size_t size) {
    return HeapAlloc(GetProcessHeap(), 0, size ? size : 1);
}
void free(void *ptr) {
    if (ptr) HeapFree(GetProcessHeap(), 0, ptr);
}
void *calloc(size_t count, size_t size) {
    if (count && size > SIZE_MAX / count) return NULL;
    size_t total = count * size;
    return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, total ? total : 1);
}
void *realloc(void *ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (!size) { free(ptr); return NULL; }
    return HeapReAlloc(GetProcessHeap(), 0, ptr, size);
}
void *memcpy(void *dest, const void *source, size_t size) {
    unsigned char *d = dest;
    const unsigned char *s = source;
    for (size_t i = 0; i < size; ++i) d[i] = s[i];
    return dest;
}
void *memmove(void *dest, const void *source, size_t size) {
    unsigned char *d = dest;
    const unsigned char *s = source;
    if ((uintptr_t)d > (uintptr_t)s && (uintptr_t)d - (uintptr_t)s < size) {
        while (size) { --size; d[size] = s[size]; }
    } else {
        for (size_t i = 0; i < size; ++i) d[i] = s[i];
    }
    return dest;
}
void *memset(void *dest, int value, size_t size) {
    unsigned char *d = dest;
    for (size_t i = 0; i < size; ++i) d[i] = (unsigned char)value;
    return dest;
}
int memcmp(const void *left, const void *right, size_t size) {
    const unsigned char *a = left, *b = right;
    for (size_t i = 0; i < size; ++i) {
        if (a[i] != b[i]) return (int)a[i] - (int)b[i];
    }
    return 0;
}
