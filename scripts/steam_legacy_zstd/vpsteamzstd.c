/* Add VSZa depot decoding to the exact legacy Steam binaries in profiles.json.
 * Use Steam's own CUtlBuffer allocator and cursor methods. No login, network,
 * license, integrity-check bypass, or graphics changes are made here. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "zstd.h"

typedef struct {
    uintptr_t element_size;
    unsigned char *memory;
    int32_t allocation_count, grow_size, get, put, max_put;
    uint16_t tab;
    uint8_t error, flags;
    void *overflow[2];
} steam_buffer;

#ifdef _WIN64
#define MODULE L"steamclient64.dll"
#define ENSURE_RVA 0xb835a0
#define SEEK_RVA 0xb85990
typedef void (*ensure_fn)(void *, int);
typedef void (*seek_fn)(void *, int, int);
_Static_assert(offsetof(steam_buffer, put) == 0x1c, "buffer ABI");
_Static_assert(offsetof(steam_buffer, flags) == 0x27, "buffer flags ABI");
static const unsigned char ensure_prefix[] = {0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20};
static const unsigned char seek_prefix[] = {0x48,0x89,0x5c,0x24,0x08};
#else
#define MODULE L"steamclient.dll"
#define ENSURE_RVA 0xa09910
#define SEEK_RVA 0xa0b880
typedef void (__attribute__((thiscall)) *ensure_fn)(void *, int);
typedef void (__attribute__((thiscall)) *seek_fn)(void *, int, int);
_Static_assert(offsetof(steam_buffer, put) == 0x14, "buffer ABI");
_Static_assert(offsetof(steam_buffer, flags) == 0x1f, "buffer flags ABI");
static const unsigned char ensure_prefix[] = {0x55,0x8b,0xec,0x8b,0x45,0x08,0x56,0x8b,0xf1,0x57};
static const unsigned char seek_prefix[] = {0x55,0x8b,0xec,0x8b,0x45,0x08,0x56,0x8b,0xf1,0x57};
#endif

static uint32_t u32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}

static uint32_t crc32(const unsigned char *p, size_t size) {
    uint32_t table[256];
    for (unsigned i=0; i<256; ++i) {
        uint32_t v=i;
        for (unsigned j=0; j<8; ++j) v=(v>>1)^((0u-(v&1))&0xedb88320u);
        table[i]=v;
    }
    uint32_t crc=0xffffffffu;
    while (size--) crc=table[(crc^*p++)&255]^(crc>>8);
    return crc^0xffffffffu;
}

/* Same four-argument cdecl / Windows x64 ABI as Steam's original decoder.
 * Steam EResult: 1=OK, 2=Fail, 25=LimitExceeded, 53=DataCorruption. */
__declspec(dllexport) int VPDecodeVSZa(const unsigned char *data, unsigned size,
        steam_buffer *buffer, unsigned max_output) {
    if (!data || !buffer || size<=23 || memcmp(data,"VSZa",4)) return 2;
    if (memcmp(data+size-3,"zsv",3)) return 53;
    uint32_t header_crc=u32(data+4), footer_crc=u32(data+size-15);
    uint64_t original_size=(uint64_t)u32(data+size-11) | (uint64_t)u32(data+size-7)<<32;
    if (header_crc!=footer_crc) return 53;
    if (original_size>max_output || original_size>(128u<<20)) return 25;
    if (buffer->put<0 || original_size>(uint64_t)(INT_MAX-buffer->put) ||
            (buffer->flags&8) || buffer->element_size!=1) return 2;
    const unsigned char *frame=data+8;
    size_t frame_size=size-23;
    size_t consumed=ZSTD_findFrameCompressedSize(frame,frame_size);
    if (ZSTD_isError(consumed) || consumed!=frame_size) return 53;
    unsigned long long declared=ZSTD_getFrameContentSize(frame,frame_size);
    if (declared==ZSTD_CONTENTSIZE_ERROR ||
            (declared!=ZSTD_CONTENTSIZE_UNKNOWN && declared!=original_size)) return 53;
    unsigned char *plain=malloc(original_size ? (size_t)original_size : 1);
    if (!plain) return 2;
    size_t written=ZSTD_decompress(plain,(size_t)original_size,frame,frame_size);
    if (ZSTD_isError(written) || written!=original_size || crc32(plain,written)!=footer_crc) {
        free(plain); return 53;
    }
    if (!written) { free(plain); return 1; }
    const unsigned char *module=(const unsigned char *)GetModuleHandleW(MODULE);
    if (!module || memcmp(module+ENSURE_RVA,ensure_prefix,sizeof(ensure_prefix)) ||
            memcmp(module+SEEK_RVA,seek_prefix,sizeof(seek_prefix))) {
        free(plain); return 2;
    }
    ensure_fn ensure=(ensure_fn)(module+ENSURE_RVA);
    seek_fn seek=(seek_fn)(module+SEEK_RVA);
    int start=buffer->put;
    ensure(buffer,start+(int)written);
    if (!buffer->memory || buffer->allocation_count<0 ||
            (size_t)buffer->allocation_count<(size_t)start+written) {
        free(plain); return 2;
    }
    memcpy(buffer->memory+start,plain,written);
    seek(buffer,1,(int)written);
    free(plain);
    return buffer->put==start+(int)written && !buffer->error ? 1 : 2;
}
