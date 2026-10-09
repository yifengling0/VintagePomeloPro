#!/usr/bin/env python3
"""Run production mapping helpers with real VMAs and injected MAP_FIXED holes.

Usage: python3 host_tests/wow64_vulkan_map_test.py --source <patched Wine>
Only the NT allocator and driver are stubbed; map recovery/range copies are
extracted from win32u/vulkan.c without rewriting their implementation.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

p = argparse.ArgumentParser()
p.add_argument('--source', type=Path, required=True)
a = p.parse_args()
s = (a.source / 'dlls/win32u/vulkan.c').read_text()
header = r'''
#define _GNU_SOURCE
#define _WIN64
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include "wine/list.h"
typedef int BOOL;
typedef uintptr_t ULONG_PTR;
typedef uintptr_t UINT_PTR;
typedef size_t SIZE_T;
typedef uint64_t VkDeviceSize;
typedef int VkResult;
#define TRUE 1
#define FALSE 0
#define VK_SUCCESS 0
#define VK_ERROR_OUT_OF_HOST_MEMORY -1
#define VK_ERROR_MEMORY_MAP_FAILED -5
#define VK_WHOLE_SIZE UINT64_MAX
#define MEM_COMMIT 1
#define MEM_RELEASE 2
#define PAGE_READWRITE 1
#define SystemEmulationBasicInformation 0
#define ERR(...) ((void)0)
#define TRACE(...) ((void)0)
#define wine_dbgstr_longlong(v) "size"
typedef struct { void *HighestUserAddress; } SYSTEM_BASIC_INFORMATION;
static int NtQuerySystemInformation(int c, void *p, int n, void *r) { return -1; }
static struct { int WowTebOffset; } teb = {1};
#define NtCurrentTeb() (&teb)
#define GetCurrentProcess() 0
static ULONG_PTR zero_bits = 0x7fffffff;
static unsigned live_maps;
static int NtAllocateVirtualMemory(int process, void **p, ULONG_PTR bits, SIZE_T *size, int t, int prot)
{
    *size = (*size + 4095) & ~4095ul;
    *p = mmap(NULL, *size, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT, -1, 0);
    if (*p == MAP_FAILED) return -1;
    assert((uintptr_t)*p + *size <= UINT32_MAX);
    live_maps++;
    return 0;
}
static SIZE_T release_size;
static int NtFreeVirtualMemory(int process, void **p, SIZE_T *size, int type)
{
    assert(!munmap(*p, release_size));
    *p = NULL; live_maps--; return 0;
}
static int inject_alias_failure;
static unsigned alias_holes;
static void *fault_mmap(void *p, size_t size, int prot, int flags, int fd, off_t offset)
{
    if (inject_alias_failure && fd >= 0 && (flags & MAP_FIXED))
    {
        assert(!munmap(p, size)); alias_holes++; errno = EINVAL; return MAP_FAILED;
    }
    return mmap(p, size, prot, flags, fd, offset);
}
#define mmap fault_mmap
typedef struct { VkDeviceSize offset, size; } VkMemoryMapInfoKHR;
typedef struct { VkDeviceSize offset, size; } VkMappedMemoryRange;
struct device_memory
{
    struct { struct { uint64_t device_memory; } host; } obj;
    VkDeviceSize size;
    void *vm_map, *host_map;
    BOOL wow64_alias, wow64_copy;
    VkDeviceSize map_offset, map_size;
    BOOL wow64_precise;
    struct list wow64_entry;
};
static struct list wow64_copy_maps = LIST_INIT(wow64_copy_maps);
static pthread_mutex_t wow64_copy_mutex = PTHREAD_MUTEX_INITIALIZER;
struct vulkan_device { void (*p_vkUnmapMemory)(void *, uint64_t); struct { void *device; } host; };
'''
# Keep the internal #ifdef in copy_range; the extracted outer closing #endif
# is removed specifically rather than altering the function.
helpers = s[s.index('static ULONG_PTR winehua_wow64_zero_bits'):s.index('static VkResult win32u_vkFlushMappedMemoryRanges')]
helpers = helpers.replace('}\n#endif\n\n/* The generated thunks', '}\n\n/* The generated thunks')
body = r'''
int main(void)
{
    const size_t pages = 8192;
    int fd = memfd_create("wow64-vulkan-regression", 0);
    assert(fd >= 0 && !ftruncate(fd, pages));
    unsigned char *host = mmap(NULL, pages, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    assert(host != MAP_FAILED && (uintptr_t)host > UINT32_MAX);
    memset(host, 0x31, pages);
    struct device_memory m = {.size = pages};
    VkMemoryMapInfoKHR info = {.offset = 0, .size = VK_WHOLE_SIZE};
    void *data = host;
    setenv("WINEHUA_VK_PRECISE_MAP", "1", 1);
    inject_alias_failure = 1;
    assert(winehua_wow64_remap_map(&m, &info, &data) == VK_SUCCESS);
    assert(alias_holes == 1 && m.wow64_copy && m.wow64_precise);
    assert(!memcmp(host, data, pages));
    memset(data, 0x72, pages);
    VkMappedMemoryRange range = {.offset = 100, .size = 200};
    assert(winehua_wow64_copy_range(&m, &range, TRUE));
    assert(host[99] == 0x31 && host[100] == 0x72 && host[299] == 0x72 && host[300] == 0x31);
    winehua_wow64_flush_copies();
    assert(host[300] == 0x31); /* no whole-map overwrite on submission */
    host[350] = 0xc3;
    range.offset = 350; range.size = 1;
    assert(winehua_wow64_copy_range(&m, &range, FALSE));
    assert(((unsigned char *)data)[350] == 0xc3);
    range.offset = pages - 10; range.size = 11;
    assert(!winehua_wow64_copy_range(&m, &range, TRUE));
    range.offset = UINT64_MAX - 5; range.size = VK_WHOLE_SIZE;
    assert(!winehua_wow64_copy_range(&m, &range, TRUE));
    release_size = pages;
    host[500] = 0xc9;
    winehua_wow64_release_map(NULL, &m);
    assert(host[500] == 0xc9 && !live_maps);
    puts("PASS destructive MAP_FIXED failure recovery; exact upload/readback ranges; no submit/unmap overwrite");

    inject_alias_failure = 0;
    info = (VkMemoryMapInfoKHR){.offset = 0, .size = VK_WHOLE_SIZE};
    data = host;
    assert(winehua_wow64_remap_map(&m, &info, &data) == VK_SUCCESS);
    assert(m.wow64_alias && !m.wow64_copy);
    ((unsigned char *)data)[77] = 0x84;
    assert(host[77] == 0x84);
    host[78] = 0x95;
    assert(((unsigned char *)data)[78] == 0x95);
    winehua_wow64_release_map(NULL, &m);
    assert(!live_maps && host[77] == 0x84);
    puts("PASS shared-file alias CPU writes and host readback without copies");

    /* Partial mapping near a guard page must not read the allocation's size. */
    unsigned char *guard = mmap(NULL, pages, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    assert(guard != MAP_FAILED && !mprotect(guard + 4096, 4096, PROT_NONE));
    memset(guard + 3968, 0xab, 128);
    m = (struct device_memory){.size = 4096};
    info = (VkMemoryMapInfoKHR){.offset = 3968, .size = VK_WHOLE_SIZE};
    data = guard + 3968;
    assert(winehua_wow64_remap_map(&m, &info, &data) == VK_SUCCESS);
    assert(m.map_size == 128 && m.map_offset == 3968);
    for (int i=0; i<128; i++) assert(((unsigned char*)data)[i] == 0xab);
    range = (VkMappedMemoryRange){.offset = 3968, .size = VK_WHOLE_SIZE};
    assert(winehua_wow64_copy_range(&m, &range, TRUE));
    range.offset = 3967;
    assert(!winehua_wow64_copy_range(&m, &range, FALSE));
    release_size = 4096;
    winehua_wow64_release_map(NULL, &m);
    munmap(guard, pages);
    info.offset = 4096; info.size = 1; data = host;
    assert(winehua_wow64_remap_map(&m, &info, &data) == VK_ERROR_MEMORY_MAP_FAILED);
    assert(!live_maps);
    puts("PASS partial map with guard page; VK_WHOLE_SIZE and overflow rejection");
    munmap(host, pages); close(fd);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='wow64-map-test-') as d:
    c, exe = Path(d)/'test.c', Path(d)/'test'
    c.write_text(header + helpers + body)
    subprocess.run(['cc', '-O2', '-I'+str(a.source/'include'), str(c), '-pthread', '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
