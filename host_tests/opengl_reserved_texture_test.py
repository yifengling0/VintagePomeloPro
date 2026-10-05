"""Replay texture-zero calls through Wine's native and WOW64 OpenGL thunks.

The predicate, generated thunk bodies, parameter layouts and shared texture
array come from Wine source. Only platform context lookup and GL calls are
stubbed. The pre-fix predicate reproduces the observed framebuffer assertion.
"""
from pathlib import Path
import os
import signal
import subprocess
import unittest

import wine_shm_state_cache_test as base

STUBS = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int BOOL, GLint, NTSTATUS;
typedef unsigned GLuint, GLenum, PTR32;
#define FALSE 0
#define TRUE 1
#define STATUS_SUCCESS 0
#define STATUS_NOT_IMPLEMENTED -1
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
struct opengl_funcs {
    void (*p_glFramebufferTexture2D)(GLenum,GLenum,GLenum,GLuint,GLint);
    void (*p_glFramebufferTexture2DEXT)(GLenum,GLenum,GLenum,GLuint,GLint);
    void (*p_glBindTexture)(GLenum,GLuint);
};
typedef struct { const struct opengl_funcs *glTable; } TEB;
'''

BOUNDARIES = r'''
struct context { struct { struct wgl_context_share *share; } base; };
static struct wgl_context_share share;
static struct context context = { { &share } };
static struct context *current = &context;
static int gl_calls, push_calls, pop_calls, attributes;
static GLuint expected;
static void attach(GLenum a,GLenum b,GLenum c,GLuint texture,GLint level) {
    assert(a==0x8d40 && b==0x8ce0 && c==0x0de1 && texture==expected && level==0);
    gl_calls++;
}
static void bind(GLenum target,GLuint texture) { assert(target==0x0de1 && texture==expected); gl_calls++; }
static const struct opengl_funcs table = { attach, attach, bind };
static TEB teb = { &table };
static struct context *get_current_context(TEB *t,void *a,void *b) { assert(t==&teb); return current; }
static TEB *get_teb64(PTR32 t) { assert(t==1); return &teb; }
static void push_default_fbo(TEB *t) { assert(t==&teb); push_calls++; }
static void pop_default_fbo(TEB *t) { assert(t==&teb); pop_calls++; }
static void set_context_attribute(TEB *t,int a,void *v,size_t s) {
    assert(t==&teb && a==-1 && !v && !s); attributes++;
}
'''

MAIN = r'''
static void dispatch(GLuint texture) {
    struct glFramebufferTexture2D_params core = { &teb,0x8d40,0x8ce0,0x0de1,texture,0 };
    struct glFramebufferTexture2DEXT_params ext = { &teb,0x8d40,0x8ce0,0x0de1,texture,0 };
    struct glBindTexture_params binding = { &teb,0x0de1,texture };
    struct { PTR32 teb; GLenum target,attachment,textarget; GLuint texture; GLint level; }
        wow = { 1,0x8d40,0x8ce0,0x0de1,texture,0 };
    struct { PTR32 teb; GLenum target; GLuint texture; } wow_binding = { 1,0x0de1,texture };
    expected=texture;
    /* Start with the exact WOW64 call that aborted War3. */
    assert(wow64_ext_glFramebufferTexture2D(&wow)==STATUS_SUCCESS);
    assert(ext_glFramebufferTexture2D(&core)==STATUS_SUCCESS);
    assert(ext_glFramebufferTexture2DEXT(&ext)==STATUS_SUCCESS);
    assert(wow64_ext_glFramebufferTexture2DEXT(&wow)==STATUS_SUCCESS);
    assert(gl_glBindTexture(&binding)==STATUS_SUCCESS);
    assert(wow64_gl_glBindTexture(&wow_binding)==STATUS_SUCCESS);
}
int main(int argc,char **argv) {
    assert(argc==2);
    if (!strcmp(argv[1],"semantics")) {
        assert(!is_wine_reserved_texture(&teb,0));
        assert(!is_wine_reserved_texture(&teb,9));
        share.framebuffer_textures[2]=7;
        assert(!is_wine_reserved_texture(&teb,0));
        assert(is_wine_reserved_texture(&teb,7));
        assert(!is_wine_reserved_texture(&teb,9));
        for (unsigned i=0;i<ARRAY_SIZE(share.framebuffer_textures);i++) share.framebuffer_textures[i]=i+20;
        for (unsigned i=0;i<ARRAY_SIZE(share.framebuffer_textures);i++) assert(is_wine_reserved_texture(&teb,i+20));
        assert(!is_wine_reserved_texture(&teb,0));
        current=NULL;
        assert(!is_wine_reserved_texture(&teb,0));
        assert(!is_wine_reserved_texture(&teb,20));
    } else if (!strcmp(argv[1],"detach")) {
        dispatch(0); /* no FBO textures allocated, as in the GL 2.1 fallback */
        share.framebuffer_textures[3]=27;
        dispatch(0); /* partially populated shares */
        for (unsigned i=0;i<ARRAY_SIZE(share.framebuffer_textures);i++) share.framebuffer_textures[i]=i+20;
        dispatch(0); /* full internal pool */
        assert(gl_calls==18 && push_calls==12 && pop_calls==12 && attributes==18);
    } else if (!strcmp(argv[1],"application")) {
        share.framebuffer_textures[3]=27;
        dispatch(9); assert(gl_calls==6);
    } else if (!strcmp(argv[1],"internal")) {
        share.framebuffer_textures[3]=27;
        dispatch(27); /* the original protection must still abort */
        abort();
    } else abort();
    puts("texture predicate and native/WOW64 dispatch PASS");
    return 0;
}
'''


class ReservedTextureTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        base.WineShmStateCacheTest.setUpClass.__func__(cls)
        wine = base.options.wine_src
        wgl = cls.first_replay['dlls/opengl32/unix_wgl.c'].decode()
        thunks = subprocess.check_output(['git','show','HEAD:dlls/opengl32/unix_thunks.c'],cwd=wine).decode()
        header = subprocess.check_output(['git','show','HEAD:dlls/opengl32/unixlib.h'],cwd=wine).decode()
        share_header = cls.first_replay['include/wine/opengl_driver.h'].decode()
        start = share_header.index('struct wgl_context_share\n{')
        shared = share_header[start:share_header.index('\n};',start)+4]
        # Keep the actual production array declaration and capacity.
        texture_field = next(line for line in shared.splitlines() if 'framebuffer_textures[' in line)
        shared = 'struct wgl_context_share {\n'+texture_field+'\n};\n'
        params = ''
        for name in ('glFramebufferTexture2D','glFramebufferTexture2DEXT','glBindTexture'):
            start = header.index('struct '+name+'_params\n{')
            params += header[start:header.index('\n};',start)+4]+'\n'
        predicate = base.function(wgl,'BOOL is_wine_reserved_texture(')
        thunk_functions = ''.join(base.function(thunks,'static NTSTATUS '+name+'(') for name in (
            'ext_glFramebufferTexture2D','ext_glFramebufferTexture2DEXT',
            'wow64_ext_glFramebufferTexture2D','wow64_ext_glFramebufferTexture2DEXT',
            'gl_glBindTexture','wow64_gl_glBindTexture'))
        old_predicate = subprocess.check_output(['git','show','HEAD:dlls/opengl32/unix_wgl.c'],cwd=wine).decode()
        old_predicate = base.function(old_predicate,'BOOL is_wine_reserved_texture(')
        cls.binaries = {}
        for name, body in (('fixed',predicate),('baseline',old_predicate)):
            path = cls.folder/(name+'-textures.c')
            path.write_text(STUBS+shared+params+BOUNDARIES+body+thunk_functions+MAIN)
            binary = cls.folder/(name+'-textures')
            subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror',
                '-Wno-unused-parameter','-Wno-address','-fsanitize=address,undefined',
                '-fno-pie','-no-pie',str(path),'-o',str(binary)],check=True)
            cls.binaries[name]=binary

    def run_case(self,scenario,name='fixed'):
        return subprocess.run([str(self.binaries[name]),scenario],capture_output=True,text=True,timeout=10,
            env=dict(os.environ,ASAN_OPTIONS='detect_leaks=1:halt_on_error=1'))

    def test_old_predicate_reproduces_device_abort(self):
        result=self.run_case('detach','baseline')
        self.assertEqual(result.returncode,-signal.SIGABRT,result.stdout+result.stderr)
        self.assertIn('wow64_ext_glFramebufferTexture2D',result.stderr)
        self.assertIn('is_wine_reserved_texture',result.stderr)

    def test_zero_default_and_shared_texture_classification(self):
        result=self.run_case('semantics')
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)

    def test_native_wow64_core_ext_and_bind_zero(self):
        result=self.run_case('detach')
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)

    def test_application_texture_and_internal_protection(self):
        result=self.run_case('application')
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        result=self.run_case('internal')
        self.assertEqual(result.returncode,-signal.SIGABRT,result.stdout+result.stderr)
        self.assertIn('wow64_ext_glFramebufferTexture2D',result.stderr)

    def test_overlays_are_idempotent(self):
        self.assertEqual(self.first_replay,self.second_replay)


if __name__=='__main__':
    unittest.main(argv=[__file__]+base.remaining)
