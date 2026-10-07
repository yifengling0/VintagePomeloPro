"""Run production upload/shader timing wrappers against argument-checking drivers."""
import os
import subprocess
import unittest

import wine_shm_state_cache_test as base
from winehua_perf_test import production_helpers, production_function

TYPES=r'''
typedef unsigned GLenum,GLuint;
typedef int GLint,GLsizei;
typedef intptr_t GLintptr,GLsizeiptr,GLintptrARB,GLsizeiptrARB;
typedef struct { const struct opengl_funcs *glTable; } TEB;
static unsigned calls;
static uint64_t delay=17;
static char bytes[16];
static GLint result;
static void tick(void) { ++calls; fake_us+=delay; }
static void subdata(GLenum target,GLintptr offset,GLsizeiptr size,const void *data) {
    assert(target==5 && offset==7 && (size==13 || size==-1) && data==bytes); tick();
}
static void image(GLenum t,GLint l,GLint i,GLsizei w,GLsizei h,GLint b,GLenum f,GLenum y,const void *p) {
    assert(t==1 && l==2 && i==3 && w==4 && h==5 && b==6 && f==7 && y==8 && p==bytes); tick();
}
static void subimage(GLenum t,GLint l,GLint x,GLint y,GLsizei w,GLsizei h,GLenum f,GLenum z,const void *p) {
    assert(t==1 && l==2 && x==3 && y==4 && w==5 && h==6 && f==7 && z==8 && p==bytes); tick();
}
static void compressed(GLenum t,GLint l,GLint x,GLint y,GLsizei w,GLsizei h,GLenum f,GLsizei z,const void *p) {
    assert(t==1 && l==2 && x==3 && y==4 && w==5 && h==6 && f==7 && z==8 && p==bytes); tick();
}
static void object(GLuint o) { assert(o==123); tick(); }
static void query(GLuint o,GLenum p,GLint *value) { assert(o==123 && p==456 && value==&result); *value=789; tick(); }
struct opengl_funcs {
    void (*p_glBufferSubData)(GLenum,GLintptr,GLsizeiptr,const void *);
    void (*p_glBufferSubDataARB)(GLenum,GLintptrARB,GLsizeiptrARB,const void *);
    void (*p_glTexImage2D)(GLenum,GLint,GLint,GLsizei,GLsizei,GLint,GLenum,GLenum,const void *);
    void (*p_glTexSubImage2D)(GLenum,GLint,GLint,GLint,GLsizei,GLsizei,GLenum,GLenum,const void *);
    void (*p_glCompressedTexSubImage2D)(GLenum,GLint,GLint,GLint,GLsizei,GLsizei,GLenum,GLsizei,const void *);
    void (*p_glCompileShader)(GLuint);
    void (*p_glLinkProgram)(GLuint);
    void (*p_glGetShaderiv)(GLuint,GLenum,GLint *);
    void (*p_glGetProgramiv)(GLuint,GLenum,GLint *);
};
'''
MAIN=r'''
int main(int argc,char **argv) {
    assert(argc==2); perf_enabled=strcmp(argv[1],"off")!=0;
    const struct opengl_funcs table={subdata,subdata,image,subimage,compressed,object,object,query,query};
    TEB teb={&table};
    wrap_glBufferSubData(&teb,5,7,13,bytes);
    wrap_glBufferSubDataARB(&teb,5,7,-1,bytes);
    wrap_glTexImage2D(&teb,1,2,3,4,5,6,7,8,bytes);
    wrap_glTexSubImage2D(&teb,1,2,3,4,5,6,7,8,bytes);
    wrap_glCompressedTexSubImage2D(&teb,1,2,3,4,5,6,7,8,bytes);
    wrap_glCompileShader(&teb,123); wrap_glLinkProgram(&teb,123);
    wrap_glGetShaderiv(&teb,123,456,&result); assert(result==789); result=0;
    wrap_glGetProgramiv(&teb,123,456,&result); assert(result==789 && calls==9);
    if (!perf_enabled) {
        assert(perf_clock_calls==0 && perf_log_count==0 && !winehua_perf_state.window_start);
    } else {
        assert(perf_clock_calls==18 && !perf_log_count);
        assert(winehua_perf_state.counters[WINEHUA_PERF_BUFFER_SUBDATA].count==2);
        assert(winehua_perf_state.counters[WINEHUA_PERF_BUFFER_SUBDATA].requested==13);
        assert(winehua_perf_state.counters[WINEHUA_PERF_BUFFER_SUBDATA].copied==0);
        for (unsigned i=WINEHUA_PERF_TEX_IMAGE;i<WINEHUA_PERF_COUNT;i++) {
            assert(winehua_perf_state.counters[i].count==1 && winehua_perf_state.counters[i].total_us==18);
        }
        memset(&winehua_perf_state,0,sizeof(winehua_perf_state)); delay=2000000;
        wrap_glCompileShader(&teb,123);
        assert(perf_log_count==1 && strstr(perf_log,"compile_shader=1/2000001/2000001/0/0/1/0/0/0/0"));
    }
    puts("Upload and shader API arguments, queries and timing PASS");
}
'''
HOOKS=('glBufferSubData','glBufferSubDataARB','glTexImage2D','glTexSubImage2D',
       'glCompressedTexSubImage2D','glCompileShader','glLinkProgram','glGetShaderiv','glGetProgramiv')

class UploadPerfTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        base.WineShmStateCacheTest.setUpClass.__func__(cls)
        source=cls.first_replay['dlls/opengl32/unix_wgl.c'].decode()
        body=production_helpers(source)+TYPES+''.join(production_function(source,'void wrap_'+f+'(') for f in HOOKS)+MAIN
        src=cls.folder/'upload.c'; cls.binary=cls.folder/'upload';src.write_text(body)
        subprocess.run(['gcc','-D_POSIX_C_SOURCE=200809L','-std=c11','-O2','-Wall','-Wextra','-Werror',
            '-Wno-unused-function','-Wno-unused-variable','-fsanitize=address,undefined','-fno-pie','-no-pie',
            str(src),'-pthread','-o',str(cls.binary)],check=True)

    def test_arguments_queries_and_no_clock_when_disabled(self):
        for mode in ('off','on'):
            r=subprocess.run([str(self.binary),mode],capture_output=True,text=True,timeout=10,
                env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0:halt_on_error=1'))
            self.assertEqual(r.returncode,0,r.stdout+r.stderr)

    def test_generated_native_and_wow64_keep_availability_checks(self):
        source=self.first_replay['dlls/opengl32/unix_thunks.c'].decode()
        for func in HOOKS:
            prefix='gl_' if func in ('glTexImage2D','glTexSubImage2D') else 'ext_'
            for wow in ('','wow64_'):
                body=base.function(source,'static NTSTATUS '+wow+prefix+func+'(')
                self.assertLess(body.index('return STATUS_NOT_IMPLEMENTED'),body.index('wrap_'+func+'('))
                self.assertEqual(body.count('wrap_'+func+'('),1)
                self.assertNotIn('p_glGetError',body)

    def test_replay_is_idempotent(self):
        self.assertEqual(self.first_replay,self.second_replay)

if __name__=='__main__':
    unittest.main(argv=[__file__]+base.remaining)
