"""Exercise D3D map flags together with the production WOW64 shadow/flush path."""
import os
import signal
import subprocess
import unittest

import wine_shm_state_cache_test as base
import opengl_wow64_buffer_flush_test as wow
from winehua_perf_test import production_helpers, production_function

TYPES = r'''
typedef unsigned DWORD;
struct wined3d_bo_gl { struct { int coherent; } b; };
#define WINED3D_MAP_READ 0x80000000u
#define WINED3D_MAP_WRITE 0x40000000u
#define WINED3D_MAP_DISCARD 0x2000
#define WINED3D_MAP_NOOVERWRITE 0x1000
#define GL_MAP_UNSYNCHRONIZED_BIT 32
'''
MAIN = r'''
int main(int argc,char **argv) {
    assert(argc==2); perf_enabled=1; teb.glTable=&table; selected=&buffer;
    driver=malloc(64); assert((uintptr_t)driver>UINT32_MAX); memset(driver,0x37,64);
    memset(gpu,0x37,64); mapped_offset=0; mapped_length=64;
    struct wined3d_bo_gl bo={0};
    unsigned flags=WINED3D_MAP_WRITE;
    if (!strcmp(argv[1],"discard")) flags|=WINED3D_MAP_DISCARD;
    else flags|=WINED3D_MAP_NOOVERWRITE;
    unsigned gl_flags=wined3d_resource_gl_map_flags(&bo,flags);
    unsigned char *p=wow64_map_buffer(&teb,&buffer,1,0,0,64,gl_flags,driver);
    assert(p && p!=driver);
    if (flags&WINED3D_MAP_DISCARD) {
        assert(winehua_perf_state.counters[WINEHUA_PERF_SHADOW_IN].count==0);
        memset(p+7,0xa5,9); wow64_glFlushMappedBufferRange(&teb,1,7,9);
        assert(gpu[7]==0xa5 && gpu[15]==0xa5);
    } else {
        assert(winehua_perf_state.counters[WINEHUA_PERF_SHADOW_IN].copied==64);
        assert(!memcmp(p,driver,64)); p[7]=0xa5;
        /* An over-wide explicit flush must preserve untouched bytes. */
        wow64_glFlushMappedBufferRange(&teb,1,3,17);
        assert(gpu[3]==0x37 && gpu[6]==0x37 && gpu[7]==0xa5 && gpu[8]==0x37);
    }
    assert(wow64_unmap_buffer(&teb,&buffer));
    /* A second NOOVERWRITE lock must see bytes published by the first. */
    gl_flags=wined3d_resource_gl_map_flags(&bo,WINED3D_MAP_WRITE|WINED3D_MAP_NOOVERWRITE);
    p=wow64_map_buffer(&teb,&buffer,1,0,0,64,gl_flags,driver);
    assert(p[7]==0xa5); p[40]=0x99;
    wow64_glFlushMappedBufferRange(&teb,1,40,1); p[7]=0xcc;
    assert(wow64_unmap_buffer(&teb,&buffer));
    assert(driver[7]==0xa5 && gpu[7]==0xa5 && gpu[40]==0x99);
    for (unsigned coherent=0;coherent<2;coherent++) {
        bo.b.coherent=coherent;
        unsigned f=wined3d_resource_gl_map_flags(&bo,WINED3D_MAP_READ|WINED3D_MAP_WRITE|WINED3D_MAP_DISCARD);
        assert((f&GL_MAP_READ_BIT) && !(f&(GL_MAP_INVALIDATE_BUFFER_BIT|GL_MAP_UNSYNCHRONIZED_BIT)));
        f=wined3d_resource_gl_map_flags(&bo,WINED3D_MAP_WRITE|WINED3D_MAP_DISCARD);
        assert(!!(f&GL_MAP_FLUSH_EXPLICIT_BIT)==!coherent);
        f=wined3d_resource_gl_map_flags(&bo,WINED3D_MAP_DISCARD);
        assert(!(f&GL_MAP_INVALIDATE_BUFFER_BIT));
    }
    free(buffer.vm_ptr); free(driver); puts("DISCARD/NOOVERWRITE shadow publication PASS");
}
'''

class DiscardMapTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        base.WineShmStateCacheTest.setUpClass.__func__(cls)
        gl=cls.first_replay['dlls/opengl32/unix_wgl.c'].decode()
        start=gl.index('struct buffer\n{')
        struct=gl[start:gl.index('\n};',start)+4]
        funcs=''.join(production_function(gl,s) for s in ('static BOOL use_driver_buffer_map(', 'static void flush_buffer(',
            'static void *wow64_map_buffer(', 'static BOOL wow64_unmap_buffer(',
            'void wow64_glFlushMappedBufferRange('))
        cls.binaries={}
        fixed=cls.first_replay['dlls/wined3d/resource.c'].decode()
        baseline=subprocess.check_output(['git','show','HEAD:dlls/wined3d/resource.c'],cwd=base.options.wine_src).decode()
        for label,resource in [('baseline',baseline),('fixed',fixed)]:
            body=base.function(resource,'GLbitfield wined3d_resource_gl_map_flags(')
            src=cls.folder/(label+'-discard.c'); binary=src.with_suffix('')
            src.write_text(production_helpers(gl)+wow.STUBS+TYPES+struct+wow.BOUNDARIES+body+funcs+MAIN)
            subprocess.run(['gcc','-D_POSIX_C_SOURCE=200809L','-std=c11','-O2','-Wall','-Wextra','-Werror',
                '-Wno-unused-function','-Wno-unused-parameter','-Wno-unused-variable',
                '-fsanitize=address,undefined','-fno-pie','-no-pie',str(src),'-pthread','-o',str(binary)],check=True)
            cls.binaries[label]=binary

    def run_case(self,label,case):
        return subprocess.run([str(self.binaries[label]),case],capture_output=True,text=True,
            env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0:halt_on_error=1'),timeout=10)

    def test_old_discard_copies_obsolete_data(self):
        r=self.run_case('baseline','discard')
        self.assertEqual(r.returncode,-signal.SIGABRT,r.stderr)
        self.assertIn('WINEHUA_PERF_SHADOW_IN',r.stderr)

    def test_discard_skips_initial_copy_but_still_publishes(self):
        r=self.run_case('fixed','discard'); self.assertEqual(r.returncode,0,r.stderr)

    def test_nooverwrite_partial_writes_preserve_other_bytes(self):
        for label in ('fixed','baseline'):
            r=self.run_case(label,'preserve'); self.assertEqual(r.returncode,0,r.stderr)

    def test_registered_overlay_replay_twice(self):
        self.assertEqual(self.first_replay,self.second_replay)

if __name__=='__main__':
    unittest.main(argv=[__file__]+base.remaining)
