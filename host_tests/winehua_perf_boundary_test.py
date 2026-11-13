"""Execute production Unix map/copy/API hooks with deterministic platform boundaries."""
import os
import subprocess
import unittest

import wine_shm_state_cache_test as base
import opengl_wow64_buffer_flush_test as wow
from winehua_perf_test import production_helpers, production_function

UNIX_TYPES=r'''
typedef int GLint, GLsizei;
typedef void *HDC;
typedef void *(*PFN_glMapBuffer)(GLenum,GLenum);
typedef void *(*PFN_glMapNamedBuffer)(GLuint,GLenum);
typedef void *(*PFN_glMapNamedBufferRange)(GLuint,GLintptr,GLsizeiptr,GLbitfield);
typedef GLboolean (*PFN_glUnmapBuffer)(GLenum);
typedef GLboolean (*PFN_glUnmapNamedBuffer)(GLuint);
#define GL_READ_ONLY 0x88b8
#define GL_WRITE_ONLY 0x88b9
#define GL_READ_WRITE 0x88ba
'''
TABLE_FIELDS=r'''
    void *(*p_glMapBuffer)(GLenum,GLenum);
    void *(*p_glMapBufferARB)(GLenum,GLenum);
    void *(*p_glMapBufferRange)(GLenum,GLintptr,GLsizeiptr,GLbitfield);
    void *(*p_glMapNamedBuffer)(GLuint,GLenum);
    void *(*p_glMapNamedBufferEXT)(GLuint,GLenum);
    void *(*p_glMapNamedBufferRange)(GLuint,GLintptr,GLsizeiptr,GLbitfield);
    void *(*p_glMapNamedBufferRangeEXT)(GLuint,GLintptr,GLsizeiptr,GLbitfield);
    GLboolean (*p_glUnmapBuffer)(GLenum);
    GLboolean (*p_glUnmapBufferARB)(GLenum);
    GLboolean (*p_glUnmapNamedBuffer)(GLuint);
    GLboolean (*p_glUnmapNamedBufferEXT)(GLuint);
    void (*p_glReadPixels)(GLint,GLint,GLsizei,GLsizei,GLenum,GLenum,void *);
    void (*p_glFinish)(void);
    void (*p_glFlush)(void);
    BOOL (*p_wglSwapBuffers)(HDC);
'''
UNIX_CALLS=r'''
static unsigned map_calls,unmap_calls,read_calls,finish_calls,flush_calls,swap_calls,pop_calls;
static int map_fail,vk_fail,swap_result=1,unmap_result=1;
static void *api_map(GLenum id,GLenum access) { assert(id==1 && access==GL_WRITE_ONLY);++map_calls;fake_us+=17;return map_fail?NULL:driver; }
static void *api_map_range(GLenum id,GLintptr offset,GLsizeiptr length,GLbitfield access) {
    assert(id==1 && offset==4 && (length==12 || length==-1 || length==INTPTR_MIN) && access==(GL_MAP_WRITE_BIT|GL_MAP_INVALIDATE_RANGE_BIT));
    ++map_calls;fake_us+=17;if(length<0) { assert(map_fail);return NULL; } return map_fail?NULL:driver+offset;
}
static GLboolean api_unmap(GLenum id) { assert(id==1);++unmap_calls;fake_us+=9;return unmap_result; }
static VkResult api_vk_map(void *device,const VkMemoryMapInfoKHR *map,void **ptr) {
    (void)device;assert(map->memory==1 && map->size==VK_WHOLE_SIZE);
    if (vk_fail) return -1;
    *ptr=((VkMemoryMapPlacedInfoEXT *)map->pNext)->pPlacedAddress;return 0;
}
static void api_read(GLint x,GLint y,GLsizei w,GLsizei h,GLenum format,GLenum type,void *ptr) {
    assert(x==1 && y==2 && w==3 && h==4 && format==5 && type==6 && ptr==driver);++read_calls;fake_us+=13;
}
static void api_finish(void) { ++finish_calls;fake_us+=11; }
static void api_flush(void) { ++flush_calls;fake_us+=3; }
static BOOL api_swap(HDC dc) { assert(dc==(HDC)7);++swap_calls;fake_us+=21;return swap_result; }
static const struct opengl_funcs *get_dc_funcs(HDC dc) { assert(dc==(HDC)7);return &table; }
static void flush_context(TEB *t,void (*f)(void)) { assert(t==&teb);if(f)f(); }
static void pop_default_fbo(TEB *t) { assert(t==&teb);++pop_calls; }
static void pop_default_fbo_buffers(TEB *t) { assert(t==&teb);++pop_calls; }
static void init(void) {
    driver=calloc(1,64);assert((uintptr_t)driver>UINT32_MAX);buffer.size=64;selected=&buffer;teb.glTable=&table;
    table.p_glMapBuffer=table.p_glMapBufferARB=table.p_glMapNamedBuffer=table.p_glMapNamedBufferEXT=api_map;
    table.p_glMapBufferRange=table.p_glMapNamedBufferRange=table.p_glMapNamedBufferRangeEXT=api_map_range;
    table.p_glUnmapBuffer=table.p_glUnmapBufferARB=table.p_glUnmapNamedBuffer=table.p_glUnmapNamedBufferEXT=api_unmap;
    table.p_glReadPixels=api_read;table.p_glFinish=api_finish;table.p_glFlush=api_flush;table.p_wglSwapBuffers=api_swap;
}
'''
UNIX_MAIN=r'''
static uint64_t count(enum winehua_perf_metric m) { return winehua_perf_state.counters[m].count; }
static uint64_t copied(enum winehua_perf_metric m) { return winehua_perf_state.counters[m].copied; }
int main(int argc,char **argv) {
    assert(argc==2);init();int enabled=perf_enabled;
    if (getenv("WINEHUA_TEST_CLOCK_FAIL")) { perf_clock_fail=1; enabled=0; }
    if (!strcmp(argv[1],"copies")) {
        unsigned char *p=wow64_map_buffer(&teb,&buffer,1,0,4,12,GL_MAP_WRITE_BIT|GL_MAP_FLUSH_EXPLICIT_BIT,driver+4);
        assert(p);memset(p,0x75,12);mapped_offset=4;mapped_length=12;
        wow64_glFlushMappedBufferRange(&teb,1,2,3);assert(gpu[6]==0x75 && gpu[8]==0x75);
        wow64_glFlushMappedNamedBufferRange(&teb,1,0,-1);
        wow64_glFlushMappedNamedBufferRangeEXT(&teb,1,12,1);
        assert(wow64_unmap_buffer(&teb,&buffer));
        p=wow64_map_buffer(&teb,&buffer,1,0,2,5,GL_MAP_WRITE_BIT|GL_MAP_INVALIDATE_RANGE_BIT,driver+2);
        memset(p,0x43,5);assert(wow64_unmap_buffer(&teb,&buffer));assert(driver[2]==0x43 && driver[6]==0x43);
        if (enabled) {
            assert(count(WINEHUA_PERF_MAP_SHADOW)==2);
            assert(winehua_perf_state.counters[WINEHUA_PERF_MAP_SHADOW].requested==17);
            assert(copied(WINEHUA_PERF_MAP_SHADOW)==0);
            assert(copied(WINEHUA_PERF_SHADOW_IN)==12 && count(WINEHUA_PERF_SHADOW_IN)==1);
            assert(copied(WINEHUA_PERF_SHADOW_FLUSH)==3 && count(WINEHUA_PERF_SHADOW_FLUSH)==1);
            assert(copied(WINEHUA_PERF_SHADOW_UNMAP)==5 && count(WINEHUA_PERF_SHADOW_UNMAP)==1);
            assert(count(WINEHUA_PERF_GL_FLUSH)==3);
        }
    } else if (!strcmp(argv[1],"paths")) {
        assert(!wow64_map_buffer(&teb,&buffer,1,0,0,8,GL_MAP_WRITE_BIT,NULL));
        void *low=(void *)(uintptr_t)0x1000;
        assert(wow64_map_buffer(&teb,&buffer,1,0,0,8,GL_MAP_WRITE_BIT,low)==low);
        assert(!wow64_map_buffer(&teb,&buffer,1,0,0,8,GL_MAP_WRITE_BIT,low));
        assert(wow64_unmap_buffer(&teb,&buffer));
        buffer.pinned=TRUE;buffer.vm_ptr=calloc(1,64);buffer.vm_size=64;
        assert(wow64_map_buffer(&teb,&buffer,1,0,3,8,GL_MAP_WRITE_BIT,NULL)==(char *)buffer.vm_ptr+3);
        assert(wow64_unmap_buffer(&teb,&buffer));buffer.pinned=FALSE;
        struct vk_device vk={.p_vkMapMemory2KHR=api_vk_map,.p_vkFlushMappedMemoryRanges=vk_flush};
        buffer.vk_memory=1;buffer.vk_device=&vk;buffers.vk_device=&vk;
        assert(wow64_map_buffer(&teb,&buffer,1,0,5,8,GL_MAP_WRITE_BIT,NULL)==(char *)buffer.vm_ptr+5);
        assert(wow64_unmap_buffer(&teb,&buffer));vk_fail=1;
        assert(!wow64_map_buffer(&teb,&buffer,1,0,5,8,GL_MAP_WRITE_BIT,NULL));
        if(enabled) {
            assert(count(WINEHUA_PERF_MAP_FAILED)==3 && count(WINEHUA_PERF_MAP_DIRECT)==1);
            assert(count(WINEHUA_PERF_MAP_PINNED)==1 && count(WINEHUA_PERF_MAP_VK)==1);
            assert(!copied(WINEHUA_PERF_SHADOW_IN) && !copied(WINEHUA_PERF_SHADOW_UNMAP));
        }
    } else if (!strcmp(argv[1],"map-aliases")) {
        assert(wow64_glMapBuffer(&teb,1,GL_WRITE_ONLY));assert(wow64_glUnmapBuffer(&teb,1));
        assert(wow64_glMapBufferARB(&teb,1,GL_WRITE_ONLY));assert(wow64_glUnmapBufferARB(&teb,1));
        assert(wow64_glMapNamedBuffer(&teb,1,GL_WRITE_ONLY));assert(wow64_glUnmapNamedBuffer(&teb,1));
        assert(wow64_glMapNamedBufferEXT(&teb,1,GL_WRITE_ONLY));assert(wow64_glUnmapNamedBufferEXT(&teb,1));
        assert(wow64_glMapBufferRange(&teb,1,4,12,GL_MAP_WRITE_BIT|GL_MAP_INVALIDATE_RANGE_BIT));assert(wow64_glUnmapBuffer(&teb,1));
        assert(wow64_glMapNamedBufferRange(&teb,1,4,12,GL_MAP_WRITE_BIT|GL_MAP_INVALIDATE_RANGE_BIT));assert(wow64_glUnmapNamedBuffer(&teb,1));
        assert(wow64_glMapNamedBufferRangeEXT(&teb,1,4,12,GL_MAP_WRITE_BIT|GL_MAP_INVALIDATE_RANGE_BIT));assert(wow64_glUnmapNamedBufferEXT(&teb,1));
        map_fail=1;assert(!wow64_glMapBuffer(&teb,1,GL_WRITE_ONLY));
        map_fail=0;assert(wow64_glMapBuffer(&teb,1,GL_WRITE_ONLY));unmap_result=0;assert(!wow64_glUnmapBuffer(&teb,1));
        assert(map_calls==9 && unmap_calls==8);
        if(enabled) {
            assert(count(WINEHUA_PERF_MAP_DRIVER)==9 && count(WINEHUA_PERF_GL_UNMAP)==8);
            assert(winehua_perf_state.counters[WINEHUA_PERF_MAP_DRIVER].results[4]==1);
            assert(winehua_perf_state.counters[WINEHUA_PERF_GL_UNMAP].results[4]==1);
        }
    } else if (!strcmp(argv[1],"negative-map")) {
        map_fail=1;
        assert(!wow64_glMapBufferRange(&teb,1,4,-1,GL_MAP_WRITE_BIT|GL_MAP_INVALIDATE_RANGE_BIT));
        assert(!wow64_glMapNamedBufferRange(&teb,1,4,INTPTR_MIN,GL_MAP_WRITE_BIT|GL_MAP_INVALIDATE_RANGE_BIT));
        assert(!wow64_glMapNamedBufferRangeEXT(&teb,1,4,-1,GL_MAP_WRITE_BIT|GL_MAP_INVALIDATE_RANGE_BIT));
        assert(map_calls==3 && !unmap_calls && !buffer.map_ptr && !buffer.host_ptr);
        if(enabled) {
            assert(count(WINEHUA_PERF_MAP_DRIVER)==3 && count(WINEHUA_PERF_MAP_FAILED)==3);
            assert(winehua_perf_state.counters[WINEHUA_PERF_MAP_FAILED].requested==0);
            assert(winehua_perf_state.counters[WINEHUA_PERF_MAP_DRIVER].results[4]==3);
            assert(!copied(WINEHUA_PERF_SHADOW_IN) && !copied(WINEHUA_PERF_SHADOW_UNMAP));
        }
    } else if (!strcmp(argv[1],"wrappers")) {
        wrap_glFinish(&teb);wrap_glReadPixels(&teb,1,2,3,4,5,6,driver);
        assert(wrap_wglSwapBuffers(&teb,(HDC)7));swap_result=0;assert(!wrap_wglSwapBuffers(&teb,(HDC)7));
        assert(finish_calls==1 && read_calls==1 && swap_calls==2 && pop_calls==2 && flush_calls==1);
        if(enabled) {
            assert(count(WINEHUA_PERF_FINISH)==1 && count(WINEHUA_PERF_READPIXELS)==1 && count(WINEHUA_PERF_WGL_SWAP)==2);
            assert(winehua_perf_state.counters[WINEHUA_PERF_WGL_SWAP].results[4]==1);
        }
    } else return 2;
    if(!enabled && !perf_clock_fail) assert(!perf_clock_calls && !perf_log_count);
    if(perf_clock_fail) assert(!perf_log_count && !winehua_perf_state.window_start);
    free(buffer.vm_ptr);free(driver);return 0;
}
'''


class PerfBoundaryTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        base.WineShmStateCacheTest.setUpClass.__func__(cls)
        cls.binaries={}
        source=cls.first_replay['dlls/opengl32/unix_wgl.c'].decode()
        start=source.index('struct buffer\n{');struct=source[start:source.index('\n};',start)+4]
        types=wow.STUBS.replace('struct opengl_funcs {','struct opengl_funcs {\n'+TABLE_FIELDS)
        # Extra typedefs must follow base's scalar declarations and precede the table.
        types=types.replace('struct opengl_funcs {',UNIX_TYPES+'\nstruct opengl_funcs {',1)
        boundary=wow.BOUNDARIES.replace('static struct opengl_funcs table = { driver_flush,driver_flush,driver_flush };',
            'static struct opengl_funcs table = {.p_glFlushMappedBufferRange=driver_flush, .p_glFlushMappedNamedBufferRange=driver_flush, .p_glFlushMappedNamedBufferRangeEXT=driver_flush};')
        signatures=('static BOOL use_driver_buffer_map(','static void flush_buffer(','static void *wow64_map_buffer(',
            'static GLbitfield map_range_flags_from_map_flags(', 'static BOOL wow64_unmap_buffer(',
            'static void *wow64_gl_map_buffer(', 'void *wow64_glMapBuffer(', 'void *wow64_glMapBufferARB(',
            'void *wow64_glMapBufferRange(', 'static void *wow64_gl_map_named_buffer(',
            'void *wow64_glMapNamedBuffer(', 'void *wow64_glMapNamedBufferEXT(', 'static void *wow64_gl_map_named_buffer_range(',
            'void *wow64_glMapNamedBufferRange(', 'void *wow64_glMapNamedBufferRangeEXT(',
            'static GLboolean wow64_unmap_target_buffer(', 'GLboolean wow64_glUnmapBuffer(', 'GLboolean wow64_glUnmapBufferARB(',
            'static GLboolean wow64_gl_unmap_named_buffer(', 'GLboolean wow64_glUnmapNamedBuffer(', 'GLboolean wow64_glUnmapNamedBufferEXT(',
            'void wow64_glFlushMappedBufferRange(', 'void wow64_glFlushMappedNamedBufferRange(', 'void wow64_glFlushMappedNamedBufferRangeEXT(',
            'void wrap_glFinish(', 'void wrap_glReadPixels(', 'BOOL wrap_wglSwapBuffers(')
        cls.compile('unix',production_helpers(source)+types+struct+boundary+UNIX_CALLS+
                    ''.join(production_function(source,s) for s in signatures)+UNIX_MAIN)

    @classmethod
    def compile(cls,name,body):
        c=cls.folder/(name+'-boundaries.c');c.write_text(body);binary=c.with_suffix('')
        subprocess.run(['gcc','-D_POSIX_C_SOURCE=200809L','-std=c11','-O2','-Wall','-Wextra','-Werror',
            '-Wno-unused-function','-Wno-unused-variable','-Wno-unused-parameter','-fsanitize=address,undefined',
            '-fno-pie','-no-pie',str(c),'-pthread','-o',str(binary)],check=True)
        cls.binaries[name]=binary

    def test_clock_failure_preserves_actual_api_calls(self):
        for case in ('copies','paths','map-aliases','negative-map','wrappers'):
            with self.subTest(case=case):
                r=subprocess.run([str(self.binaries['unix']),case],capture_output=True,text=True,timeout=10,
                    env=dict(os.environ,WINEHUA_TEST_PERF='1',WINEHUA_TEST_CLOCK_FAIL='1',ASAN_OPTIONS='detect_leaks=0:halt_on_error=1'))
                self.assertEqual(r.returncode,0,r.stdout+r.stderr)

    def test_unix_actual_copy_paths_and_api_arguments_off_on(self):
        for enabled in (0,1):
            for case in ('copies','paths','map-aliases','negative-map','wrappers'):
                with self.subTest(enabled=enabled,case=case):
                    r=subprocess.run([str(self.binaries['unix']),case],capture_output=True,text=True,timeout=10,
                        env=dict(os.environ,WINEHUA_TEST_PERF=str(enabled),ASAN_OPTIONS='detect_leaks=0:halt_on_error=1'))
                    self.assertEqual(r.returncode,0,r.stdout+r.stderr)


if __name__=='__main__':
    unittest.main(argv=[__file__]+base.remaining)
