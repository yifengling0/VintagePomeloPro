"""Exercise real capability and drawable selection across context changes.

Replays the registered overlays twice. GL/Win32 entry points are boundaries;
context capability, FBO selection and cached drawable invalidation are production
functions. This is not a device rendering test.
"""
import os
import subprocess
import unittest

import wine_shm_state_cache_test as base

STUBS = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#define NULL 0
typedef int BOOL, HWND, HDC, LONG;
#define TRUE 1
#define FALSE 0
#define GAMMA_RAMP_SIZE 256
#define MDT_RAW_DPI 0
#define TRACE(...) ((void)0)
struct wgl_context { int format, fshack_capable; unsigned gamma_program, gamma_ramp; int fshack_caps_checked; };
struct context { int major_version, minor_version; const char *extensions; struct wgl_context base; };
struct opengl_funcs { int unused; };
static int shader_calls, shader_fail;
static void fs_hack_setup_gamma_shader(struct wgl_context *c, const void *f) {
    shader_calls++; if (!shader_fail) { c->gamma_program=1; c->gamma_ramp=2; }
}
static BOOL is_extension_supported(struct context *c,const char *e) { return !strcmp(c->extensions,e); }
static int fshack_enabled=1, dpi=96, raw_dpi=96, gamma;
static BOOL wm(const char *name) { return FALSE; }
static struct { BOOL (*pHasWindowManager)(const char *); } user_table={wm}, *user_driver=&user_table;
static int NtUserGetDpiForWindow(HWND h) { return dpi; }
static int NtUserGetWinMonitorDpi(HWND h,int type) { return raw_dpi; }
static BOOL get_float_gamma_ramp(float *r,LONG *s) { return gamma; }
static const int framebuffer_surface_funcs=1, normal_funcs=2;
struct client_surface { HWND hwnd; };
struct opengl_drawable { int format, refs; const int *funcs; struct client_surface *client; HDC owner_hdc; };
typedef struct { struct opengl_drawable *unused_drawable; } WND;
static WND window;
#define WND_DESKTOP ((WND*)1)
#define WND_OTHER_PROCESS ((WND*)2)
static struct client_surface client={1};
static struct opengl_drawable *dc_drawable;
static int fbo_created, normal_created;
static WND *get_win_ptr(HWND h) { return &window; }
static void release_win_ptr(WND *w) {}
static void opengl_drawable_add_ref(struct opengl_drawable *d) { assert(d && d->refs>0); d->refs++; }
static void opengl_drawable_release(struct opengl_drawable *d) { assert(d && d->refs>0); if (!--d->refs) free(d); }
static void surface_create(HWND h,BOOL raw,int format,struct opengl_drawable **d) {
    *d=calloc(1,sizeof(**d)); **d=(struct opengl_drawable){format,1,&normal_funcs,&client,0}; normal_created++;
}
static struct { void (*p_surface_create)(HWND,BOOL,int,struct opengl_drawable **); } driver_table={surface_create}, *driver_funcs=&driver_table;
static void add_window_client_surface(HWND h,struct client_surface *c) { assert(h==c->hwnd); }
static struct opengl_drawable *framebuffer_surface_create(int format,struct client_surface *c,struct opengl_drawable *target) {
    struct opengl_drawable *d=calloc(1,sizeof(*d)); *d=(struct opengl_drawable){format,1,&framebuffer_surface_funcs,c,0};
    fbo_created++; return d;
}
static HWND NtUserWindowFromDC(HDC dc) { return dc; }
static BOOL is_client_surface_window(struct client_surface *c,HWND h) { return c && c->hwnd==h; }
static struct opengl_drawable *get_dc_opengl_drawable(HDC dc) { if (dc_drawable) opengl_drawable_add_ref(dc_drawable); return dc_drawable; }
static void set_dc_opengl_drawable(HDC dc,struct opengl_drawable *d) {
    if (d) opengl_drawable_add_ref(d);
    if (dc_drawable) opengl_drawable_release(dc_drawable);
    dc_drawable=d;
}
'''

MAIN = r'''
int main(int argc,char **argv) {
    struct wgl_context legacy={1,0}, modern={1,1}, bootstrap={1,0};
    struct context c={2,1,"GL_EXT_direct_state_access"};
    BOOL updated;
    struct opengl_drawable *d,*n;
    if (!strcmp(argv[1],"caps")) {
        assert(!fs_hack_context_supported(&c));
        c=(struct context){3,2,"GL_ARB_direct_state_access"}; assert(!fs_hack_context_supported(&c));
        c=(struct context){3,3,"GL_EXT_direct_state_access"}; assert(!fs_hack_context_supported(&c));
        c=(struct context){3,3,"GL_ARB_direct_state_access"}; assert(fs_hack_context_supported(&c));
        c=(struct context){4,3,"GL_ARB_direct_state_access"}; assert(fs_hack_context_supported(&c));
        c=(struct context){4,5,""}; assert(fs_hack_context_supported(&c));
    } else if (!strcmp(argv[1],"gamma-switch")) {
        gamma=1;
        d=get_window_unused_drawable(1,1,&bootstrap); assert(d->funcs==&normal_funcs && !fbo_created);
        n=get_updated_drawable(1,&modern,d,&updated); assert(updated && n->funcs==&framebuffer_surface_funcs);
        opengl_drawable_release(d); d=n;
        n=get_updated_drawable(1,&legacy,d,&updated); assert(updated && n->funcs==&normal_funcs);
        opengl_drawable_release(d); d=n;
        for(int i=0;i<200;i++) { n=get_updated_drawable(1,&legacy,d,&updated); assert(!updated && n==d); opengl_drawable_release(n); }
        assert(fbo_created==1); opengl_drawable_release(d);
    } else if (!strcmp(argv[1],"cached-dpi")) {
        dpi=144;
        d=get_window_unused_drawable(1,1,&modern); assert(d->funcs==&framebuffer_surface_funcs);
        window.unused_drawable=d;
        n=get_window_unused_drawable(1,1,&legacy); assert(n->funcs==&normal_funcs && !window.unused_drawable);
        set_dc_opengl_drawable(1,n); opengl_drawable_release(n);
        n=get_updated_drawable(1,&modern,NULL,&updated); assert(updated && n->funcs==&framebuffer_surface_funcs);
        opengl_drawable_release(n);
        n=get_updated_drawable(1,&legacy,NULL,&updated); assert(updated && n->funcs==&normal_funcs);
        opengl_drawable_release(n); set_dc_opengl_drawable(1,NULL);
    } else if (!strcmp(argv[1],"gamma-reset")) {
        gamma=1; d=get_window_unused_drawable(1,1,&modern); assert(d->funcs==&framebuffer_surface_funcs);
        gamma=0; n=get_updated_drawable(1,&modern,d,&updated); assert(updated && n->funcs==&normal_funcs);
        opengl_drawable_release(d); opengl_drawable_release(n);
        gamma=1; fshack_enabled=0;
        assert(!needs_framebuffer_surface(1,&modern)); assert(!needs_framebuffer_surface(1,NULL));
    } else if (!strcmp(argv[1],"shader-failure")) {
        fs_hack_init_context(&c,NULL); assert(!shader_calls && !c.base.fshack_capable);
        c=(struct context){4,5,""}; shader_fail=1;
        fs_hack_init_context(&c,NULL); assert(shader_calls==1 && !c.base.fshack_capable);
        fs_hack_init_context(&c,NULL); assert(shader_calls==1); /* no per-frame retry */
        c.base.fshack_caps_checked=0; shader_fail=0;
        fs_hack_init_context(&c,NULL); assert(shader_calls==2 && c.base.fshack_capable);
    } else abort();
    puts("capability and drawable selection PASS"); return 0;
}
'''


class FullscreenCapabilityTest(base.WineShmStateCacheTest):
    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        gl = cls.first_replay['dlls/win32u/opengl.c'].decode()
        wgl = cls.first_replay['dlls/opengl32/unix_wgl.c'].decode()
        functions = base.function(wgl, 'static BOOL fs_hack_context_supported(')
        functions += base.function(wgl, 'static void fs_hack_init_context(')
        functions += ''.join(base.function(gl, sig) for sig in (
            'static BOOL needs_framebuffer_surface(',
            'static struct opengl_drawable *get_window_unused_drawable(',
            'static BOOL needs_framebuffer_update(',
            'static struct opengl_drawable *get_updated_drawable('))
        source = cls.folder/'fshack.c'
        source.write_text(STUBS+functions+MAIN)
        cls.fshack_binary = cls.folder/'fshack'
        subprocess.run(['gcc','-D__OHOS__','-std=c11','-Wall','-Wextra','-Werror',
            '-Wno-unused-parameter','-Wno-missing-field-initializers','-fsanitize=address,undefined','-fno-pie','-no-pie',
            str(source),'-o',str(cls.fshack_binary)],check=True)

    def test_real_drawable_transitions(self):
        for scenario in ('caps','gamma-switch','cached-dpi','gamma-reset','shader-failure'):
            with self.subTest(scenario=scenario):
                result = subprocess.run([str(self.fshack_binary),scenario],capture_output=True,text=True,timeout=10,
                    env=dict(os.environ,ASAN_OPTIONS='detect_leaks=1:halt_on_error=1'))
                self.assertEqual(result.returncode,0,result.stdout+result.stderr)


if __name__ == '__main__':
    unittest.main(argv=[__file__]+base.remaining)
