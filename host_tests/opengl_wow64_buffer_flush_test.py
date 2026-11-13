"""Exercise production WOW64 map/flush/unmap with a driver that snapshots flushes."""
import os
import signal
import subprocess
import unittest

import wine_shm_state_cache_test as base
from winehua_perf_test import production_helpers, production_function

STUBS = r'''
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int BOOL, VkResult;
typedef unsigned GLuint, GLenum, GLbitfield;
typedef unsigned char GLboolean;
typedef size_t SIZE_T;
typedef intptr_t GLintptr, GLsizeiptr;
typedef uintptr_t VkDeviceMemory;
typedef struct { const struct opengl_funcs *glTable; } TEB;
struct rb_entry { uintptr_t unused; };
#define TRUE 1
#define FALSE 0
#define TRACE(...) ((void)0)
#define ERR(...) ((void)0)
#define FIXME(...) ((void)0)
#define GL_MAP_READ_BIT 1
#define GL_MAP_WRITE_BIT 2
#define GL_MAP_INVALIDATE_RANGE_BIT 4
#define GL_MAP_INVALIDATE_BUFFER_BIT 8
#define GL_MAP_FLUSH_EXPLICIT_BIT 16
#define GL_MAP_PERSISTENT_BIT 64
#define GL_INVALID_OPERATION 0x502
#define GL_BUFFER_SIZE 0x8764
#define VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE 1
#define VK_STRUCTURE_TYPE_MEMORY_MAP_PLACED_INFO_EXT 2
#define VK_STRUCTURE_TYPE_MEMORY_MAP_INFO_KHR 3
#define VK_MEMORY_MAP_PLACED_BIT_EXT 1
#define VK_WHOLE_SIZE (~(size_t)0)
#define PtrToUlong(p) ((uint32_t)(uintptr_t)(p))
#define ULongToPtr(p) ((void *)(uintptr_t)(uint32_t)(p))
typedef struct { int sType; VkDeviceMemory memory; size_t offset, size; } VkMappedMemoryRange;
typedef struct { int sType; void *pPlacedAddress; } VkMemoryMapPlacedInfoEXT;
typedef struct { int sType; unsigned flags; void *pNext; VkDeviceMemory memory; size_t size; } VkMemoryMapInfoKHR;
struct vk_device {
    void *vk_device;
    VkResult (*p_vkFlushMappedMemoryRanges)(void *,unsigned,const VkMappedMemoryRange *);
    VkResult (*p_vkMapMemory2KHR)(void *,const VkMemoryMapInfoKHR *,void **);
};
struct buffers { int map; struct vk_device *vk_device; };
struct context { struct buffers *buffers; };
static struct buffers buffers;
static struct context context = { &buffers };
static TEB teb;
static unsigned error, gl_flushes, vk_flushes;
static unsigned char *driver, gpu[64];
static size_t last_offset, last_length, mapped_offset, mapped_length;
static pthread_mutex_t wgl_lock = PTHREAD_MUTEX_INITIALIZER;
struct opengl_funcs {
    void (*p_glFlushMappedBufferRange)(GLenum,GLintptr,GLsizeiptr);
    void (*p_glFlushMappedNamedBufferRange)(GLuint,GLintptr,GLsizeiptr);
    void (*p_glFlushMappedNamedBufferRangeEXT)(GLuint,GLintptr,GLsizeiptr);
};
'''

BOUNDARIES = r'''
static struct buffer buffer;
static struct buffer *selected;
static struct context *get_current_context(TEB *t,void *a,void *b) { return &context; }
static void set_gl_error(TEB *t,unsigned e) { error=e; }
static struct buffer *get_target_buffer(TEB *t,GLenum target) { return selected; }
static struct buffer *get_named_buffer(TEB *t,GLuint name) { return selected; }
static GLuint get_target_name(TEB *t,GLenum target) { return 1; }
static size_t get_named_buffer_param(TEB *t,GLuint name,GLenum p) { return 64; }
static size_t get_buffer_param(TEB *t,GLenum target,GLenum p) { return 64; }
static void rb_put(int *tree,GLuint *name,struct rb_entry *entry) {}
static BOOL buffer_vm_alloc(TEB *t,struct buffer *b,SIZE_T size) {
    if (b->vm_size<size) { free(b->vm_ptr); b->vm_ptr=calloc(1,size); b->vm_size=size; }
    return !!b->vm_ptr;
}
static void unmap_buffer(TEB *t,GLenum target) {}
static void unmap_named_buffer(TEB *t,GLuint name) {}
static void unmap_vk_buffer(struct buffer *b) {}
static void driver_flush(unsigned id,GLintptr offset,GLsizeiptr length) {
    gl_flushes++; last_offset=offset; last_length=length;
    if (offset<0 || length<0 || (size_t)offset>mapped_length || (size_t)length>mapped_length-offset) return;
    memcpy(gpu+mapped_offset+offset,driver+mapped_offset+offset,length);
}
static VkResult vk_flush(void *device,unsigned n,const VkMappedMemoryRange *range) {
    assert(n==1); vk_flushes++; last_offset=range->offset; last_length=range->size; return 0;
}
static struct opengl_funcs table = { driver_flush,driver_flush,driver_flush };
'''

MAIN = r'''
static void reset(void) {
    teb.glTable=&table;
    free(buffer.vm_ptr); memset(&buffer,0,sizeof(buffer)); selected=&buffer;
    if (!driver) driver=malloc(64);
    assert((uintptr_t)driver>UINT32_MAX);
    memset(driver,0,64); memset(gpu,0,sizeof(gpu));
    gl_flushes=vk_flushes=error=0; mapped_offset=mapped_length=0;
}
static void *map(GLintptr offset,size_t length,GLbitfield access) {
    mapped_offset=offset; mapped_length=length;
    return wow64_map_buffer(&teb,&buffer,1,0,offset,length,access,driver+offset);
}
int main(int argc,char **argv) {
    assert(argc==2); reset();
    if (!strcmp(argv[1],"explicit")) {
        unsigned char *p=map(5,24,GL_MAP_WRITE_BIT|GL_MAP_INVALIDATE_RANGE_BIT|GL_MAP_FLUSH_EXPLICIT_BIT);
        assert(p && p!=driver+5); memset(p,0xa5,24);
        for (int alias=0;alias<3;alias++) {
            if (alias==0) wow64_glFlushMappedBufferRange(&teb,1,3,5);
            if (alias==1) wow64_glFlushMappedNamedBufferRange(&teb,1,3,5);
            if (alias==2) wow64_glFlushMappedNamedBufferRangeEXT(&teb,1,3,5);
            assert(gpu[7]==0 && gpu[8]==0xa5 && gpu[12]==0xa5 && gpu[13]==0);
        }
        assert(gl_flushes==3);
        /* Unflushed edits must not overwrite the data already published. */
        p[3]=0xcc; assert(wow64_unmap_buffer(&teb,&buffer));
        assert(driver[8]==0xa5 && driver[5]==0 && gpu[8]==0xa5);
        assert(!buffer.map_ptr && !buffer.host_ptr && !buffer.copy_length);
    } else if (!strcmp(argv[1],"implicit")) {
        unsigned char *p=map(7,16,GL_MAP_WRITE_BIT|GL_MAP_INVALIDATE_RANGE_BIT);
        memset(p,0x51,16); assert(wow64_unmap_buffer(&teb,&buffer));
        assert(driver[6]==0 && driver[7]==0x51 && driver[22]==0x51 && driver[23]==0);
        p=map(1,8,GL_MAP_WRITE_BIT|GL_MAP_FLUSH_EXPLICIT_BIT); p[2]=0x42;
        wow64_glFlushMappedBufferRange(&teb,1,2,1); assert(gpu[3]==0x42);
        assert(wow64_unmap_buffer(&teb,&buffer));
        p=map(1,8,GL_MAP_WRITE_BIT); p[0]=0x77;
        assert(wow64_unmap_buffer(&teb,&buffer) && driver[1]==0x77);
    } else if (!strcmp(argv[1],"bounds")) {
        unsigned char *p=map(4,12,GL_MAP_WRITE_BIT|GL_MAP_INVALIDATE_RANGE_BIT|GL_MAP_FLUSH_EXPLICIT_BIT);
        memset(p,0x99,12);
        wow64_glFlushMappedBufferRange(&teb,1,-1,1);
        wow64_glFlushMappedBufferRange(&teb,1,0,-1);
        wow64_glFlushMappedBufferRange(&teb,1,12,1);
        wow64_glFlushMappedNamedBufferRange(&teb,1,7,6);
        wow64_glFlushMappedNamedBufferRangeEXT(&teb,1,INTPTR_MAX,INTPTR_MAX);
        wow64_glFlushMappedBufferRange(&teb,1,12,0);
        for (size_t i=0;i<64;i++) assert(driver[i]==0);
        wow64_glFlushMappedBufferRange(&teb,1,10,2);
        assert(gpu[14]==0x99 && gpu[15]==0x99 && gpu[13]==0);
        assert(wow64_unmap_buffer(&teb,&buffer));
    } else if (!strcmp(argv[1],"readonly")) {
        driver[4]=0x73; unsigned char *p=map(4,12,GL_MAP_READ_BIT);
        assert(p[0]==0x73); p[0]=0x66;
        assert(wow64_unmap_buffer(&teb,&buffer) && driver[4]==0x73);
        selected=NULL; wow64_glFlushMappedBufferRange(&teb,1,0,1); assert(gl_flushes==1);
    } else if (!strcmp(argv[1],"direct-vulkan")) {
        /* Direct low-address and pinned/Vulkan mappings have no shadow copy. */
        void *low=(void *)(uintptr_t)0x1000;
        assert(wow64_map_buffer(&teb,&buffer,1,0,0,8,GL_MAP_WRITE_BIT,low)==low);
        assert(wow64_unmap_buffer(&teb,&buffer));
        buffer.pinned=TRUE; buffer.vm_ptr=malloc(32); buffer.vm_size=32;
        assert(wow64_map_buffer(&teb,&buffer,1,0,3,8,GL_MAP_WRITE_BIT,NULL)==(char *)buffer.vm_ptr+3);
        assert(wow64_unmap_buffer(&teb,&buffer));
        struct vk_device device={ .p_vkFlushMappedMemoryRanges=vk_flush };
        buffer.vk_memory=1; buffer.vk_device=&device;
        buffer.host_ptr=driver; buffer.map_ptr=driver+7;
        flush_buffer(&teb,&buffer,2,4); assert(vk_flushes==1 && last_offset==9 && last_length==4);
        assert(wow64_unmap_buffer(&teb,&buffer));
    } else abort();
    free(buffer.vm_ptr); free(driver); puts("WOW64 buffer publication PASS"); return 0;
}
'''


class Wow64BufferFlushTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        base.WineShmStateCacheTest.setUpClass.__func__(cls)
        fixed = cls.first_replay['dlls/opengl32/unix_wgl.c'].decode()
        baseline = subprocess.check_output(['git','show','HEAD:dlls/opengl32/unix_wgl.c'],
                                            cwd=base.options.wine_src).decode()
        start = fixed.index('struct buffer\n{')
        struct = fixed[start:fixed.index('\n};',start)+4]
        signatures = ('static BOOL use_driver_buffer_map(', 'static void flush_buffer(',
            'static void *wow64_map_buffer(', 'static BOOL wow64_unmap_buffer(',
            'void wow64_glFlushMappedBufferRange(', 'void wow64_glFlushMappedNamedBufferRange(',
            'void wow64_glFlushMappedNamedBufferRangeEXT(')
        cls.binaries = {}
        for name,source in (('baseline',baseline),('fixed',fixed)):
            body = ''.join(production_function(source,signature) for signature in signatures)
            file = cls.folder/(name+'-buffer.c')
            file.write_text(production_helpers(fixed)+STUBS+struct+BOUNDARIES+body+MAIN)
            binary = cls.folder/(name+'-buffer')
            subprocess.run(['gcc','-D_POSIX_C_SOURCE=200809L','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-unused-function','-Wno-unused-variable',
                '-fsanitize=address,undefined','-fno-pie','-no-pie',str(file),'-pthread','-o',str(binary)],check=True)
            cls.binaries[name]=binary

    def run_case(self,case,name='fixed',enabled=False):
        return subprocess.run([str(self.binaries[name]),case],capture_output=True,text=True,timeout=10,
                              env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0:halt_on_error=1',WINEHUA_TEST_PERF=str(int(enabled))))

    def test_baseline_flush_publishes_stale_bytes(self):
        result=self.run_case('explicit','baseline')
        self.assertEqual(result.returncode,-signal.SIGABRT,result.stdout+result.stderr)
        self.assertIn('gpu[8]==0xa5',result.stderr)

    def test_explicit_flush_order_offset_aliases_and_unmap(self):
        result=self.run_case('explicit'); self.assertEqual(result.returncode,0,result.stdout+result.stderr)

    def test_implicit_copyback_and_mapping_reuse(self):
        result=self.run_case('implicit'); self.assertEqual(result.returncode,0,result.stdout+result.stderr)

    def test_bad_ranges_do_not_copy_or_overflow(self):
        result=self.run_case('bounds'); self.assertEqual(result.returncode,0,result.stdout+result.stderr)

    def test_readonly_untracked_direct_pinned_and_vulkan(self):
        for case in ('readonly','direct-vulkan'):
            with self.subTest(case=case):
                result=self.run_case(case); self.assertEqual(result.returncode,0,result.stdout+result.stderr)

    def test_diagnostics_on_preserves_0035_cases(self):
        for case in ('explicit','implicit','bounds','readonly','direct-vulkan'):
            with self.subTest(case=case):
                result=self.run_case(case,enabled=True)
                self.assertEqual(result.returncode,0,result.stdout+result.stderr)

    def test_registered_patch_replay_is_idempotent(self):
        self.assertEqual(self.first_replay,self.second_replay)


if __name__=='__main__':
    unittest.main(argv=[__file__]+base.remaining)
