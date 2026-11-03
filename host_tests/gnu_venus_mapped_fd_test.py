# SPDX-License-Identifier: LGPL-2.1-or-later
"""Exercise the actual locked Venus private alias bodies, without a GPU.

Run after preparing Mesa. This checks descriptor ownership and shared pages;
it is not a substitute for WoW64/GPU visibility qualification on a device.
"""
from pathlib import Path
import os
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]
TASK = ROOT/'build/host_tests/gnu-venus-mapped-fd'
SOURCE = Path(os.environ.get('VPP_GNU_MESA_TEST_SOURCE', str(
    ROOT/'build/gnu-venus-overlays/source')))

PREAMBLE = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#define VK_NULL_HANDLE 0
#define VCMD_BLOB_FLAG_SHAREABLE 2
#define container_of(ptr,type,member) ((type*)((char*)(ptr)-offsetof(type,member)))
typedef uintptr_t VkDeviceMemory;
struct vn_renderer;
struct vn_renderer_bo { void *mmap_ptr; size_t mmap_size; };
struct vtest_bo { struct vn_renderer_bo base; unsigned blob_flags; int res_fd; };
struct vn_renderer { struct { int (*dup_mapped_fd)(struct vn_renderer*,struct vn_renderer_bo*); } bo_ops; };
struct vk_device { int dummy; };
struct vn_device { struct { struct vk_device base; } base; struct vn_renderer *renderer; };
struct vn_device_memory {
    struct { struct { struct { struct vk_device *device; } base; uint64_t size; } base; } base;
    bool mapped;
    struct vn_renderer_bo *base_bo;
};
static struct vn_device_memory *vn_device_memory_from_handle(VkDeviceMemory h) { return (void*)h; }
static bool fail_dup;
static int os_dupfd_cloexec(int fd) { if(fail_dup){errno=EMFILE;return -1;}return fcntl(fd,F_DUPFD_CLOEXEC,0); }
'''

MAIN = r'''
static unsigned fds(void) {
    DIR *d=opendir("/proc/self/fd");assert(d);unsigned n=0;
    while(readdir(d))++n;closedir(d);return n;
}
int main(void) {
    alarm(10);
    uint64_t size=42;
    assert(vn_winehua_mapped_memory_fd(0,&size)==-1&&errno==EINVAL&&size==0);
    assert(vn_winehua_mapped_memory_fd(0,NULL)==-1&&errno==EINVAL);
    struct vn_renderer renderer={.bo_ops.dup_mapped_fd=vtest_bo_dup_mapped_fd};
    struct vn_device dev={.renderer=&renderer};
    struct vn_device_memory mem={.base.base.base.device=&dev.base.base,.base.base.size=4096};
    VkDeviceMemory handle=(uintptr_t)&mem;
    assert(vn_winehua_mapped_memory_fd(handle,&size)==-1&&errno==ENOTSUP&&size==0);
    unsigned before=fds();
    for(unsigned i=0;i<100;++i) {
        int original=memfd_create("hsl-alias-contract",MFD_CLOEXEC);assert(original>=0);
        assert(!ftruncate(original,4096));
        void *high=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,original,0);assert(high!=MAP_FAILED);
        struct vtest_bo bo={.base={high,4096},.res_fd=original};
        mem.base_bo=&bo.base;mem.mapped=true;
        // Ordinary BOs remain ineligible for Vulkan external dma-buf export.
        assert(vtest_bo_export_dma_buf(&renderer,&bo.base)==-1);
        fail_dup=true;size=42;
        assert(vn_winehua_mapped_memory_fd(handle,&size)==-1&&errno==EMFILE&&size==0);
        fail_dup=false;
        int alias_fd=vn_winehua_mapped_memory_fd(handle,&size);assert(alias_fd>=0&&alias_fd!=original&&size==4096);
        assert(fcntl(alias_fd,F_GETFD)&FD_CLOEXEC);
        void *alias=mmap(NULL,size,PROT_READ|PROT_WRITE,MAP_SHARED,alias_fd,0);assert(alias!=MAP_FAILED);
        close(alias_fd);
        ((uint32_t*)alias)[0]=i+1;assert(((uint32_t*)high)[0]==i+1);
        ((uint32_t*)high)[1]=i+2;assert(((uint32_t*)alias)[1]==i+2);
        mem.mapped=false;
        assert(vn_winehua_mapped_memory_fd(handle,&size)==-1&&errno==ENOTSUP&&size==0);
        close(original); // The alias mapping owns its reference to the pages.
        assert(((uint32_t*)alias)[0]==i+1);
        assert(!munmap(alias,4096));assert(!munmap(high,4096));
    }
    assert(fds()==before);
    puts("PASS: 100 shared aliases, bidirectional visibility, CLOEXEC, no fd growth, unmapped/error refusal; no external-memory capability change");
}
'''


class MappedFdTests(unittest.TestCase):
    def test_private_alias_descriptor_contract(self):
        vtest = (SOURCE/'src/virtio/vulkan/vn_renderer_vtest.c').read_text()
        memory = (SOURCE/'src/virtio/vulkan/vn_device_memory.c').read_text()
        export = vtest[vtest.index('static int\nvtest_bo_export_dma_buf('):
                       vtest.index('\nstatic bool\nvtest_bo_destroy(')]
        alias = memory[memory.index('__attribute__((visibility("default"))) int\nvn_winehua_mapped_memory_fd('):
                       memory.index('\nvoid\nvn_UnmapMemory(')]
        TASK.mkdir(parents=True, exist_ok=True)
        generated = TASK/'contract.c'
        generated.write_text(PREAMBLE+export+alias+MAIN)
        binary = TASK/'contract'
        subprocess.run(['cc','-std=gnu11','-O2','-Wall','-Wextra','-Werror',
                        '-Wno-unused-parameter','-Wno-misleading-indentation',
                        generated,'-o',binary],check=True,timeout=30)
        result = subprocess.run([binary],text=True,capture_output=True,timeout=15)
        (TASK/'result.log').write_text(result.stdout+result.stderr)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        print(result.stdout,end='')


if __name__ == '__main__':
    unittest.main()
