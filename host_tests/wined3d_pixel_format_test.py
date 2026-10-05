"""Run the complete production GL present branch with compact WGL format arrays."""
import os
import subprocess
import unittest

import wine_shm_state_cache_test as base

STUBS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
typedef struct { int left, top, right, bottom; } RECT;
#define WARN(...) ((void)0)
#define WARN_(x) WARN
#define TRACE(...) ((void)0)
#define WGL_SWAP_COPY_ARB 1
#define WINED3D_LOCATION_DRAWABLE 4u
enum wined3d_swap_effect { WINED3D_SWAP_EFFECT_DISCARD, WINED3D_SWAP_EFFECT_COPY, WINED3D_SWAP_EFFECT_COPY_VSYNC };
struct wined3d_pixel_format { int iPixelFormat, swap_method; };
struct wined3d_adapter_gl { struct wined3d_pixel_format *pixel_formats; unsigned pixel_format_count; };
struct wined3d_swapchain_desc { enum wined3d_swap_effect swap_effect; unsigned windowed, backbuffer_width, backbuffer_height; };
struct wined3d_texture { struct { unsigned draw_binding; } resource; };
struct wined3d_device { struct wined3d_adapter_gl *adapter; unsigned context_count; void *backup_dc; };
struct wined3d_swapchain { struct wined3d_texture **back_buffers, *front_buffer; struct wined3d_device *device; struct { struct wined3d_swapchain_desc desc; } state; void *win_handle; };
struct wined3d_context { struct { int fences; } *d3d_info; };
struct wined3d_gl_info { struct { struct { void (*p_glFinish)(void); } gl; struct { void (*p_wglSwapBuffers)(void *); } wgl; } gl_ops; };
struct wined3d_context_gl { struct wined3d_context c; int valid, pixel_format; void *dc; const struct wined3d_gl_info *gl_info; };
static unsigned acquire, release, gdi, blit, load, interval, finish, swap, fence, rotate, validate, invalidate;
static struct wined3d_context_gl ctx;
static struct wined3d_context *context_acquire(struct wined3d_device *d,struct wined3d_texture *t,unsigned sub) { ++acquire; return &ctx.c; }
static void context_release(struct wined3d_context *c) { assert(c==&ctx.c); ++release; }
static struct wined3d_context_gl *wined3d_context_gl(struct wined3d_context *c) { return &ctx; }
static struct wined3d_adapter_gl *wined3d_adapter_gl(struct wined3d_adapter_gl *a) { return a; }
static struct wined3d_device *wined3d_device_gl(struct wined3d_device *d) { return d; }
static void GetClientRect(void *window,RECT *r) { *r=(RECT){0,0,640,480}; }
static void swapchain_blit_gdi(struct wined3d_swapchain *s,struct wined3d_context *c,const RECT *a,const RECT *b) { ++gdi; }
static void swapchain_blit(struct wined3d_swapchain *s,struct wined3d_context *c,const RECT *a,const RECT *b) { ++blit; }
static void swapchain_gl_set_swap_interval(struct wined3d_swapchain *s,struct wined3d_context_gl *c,unsigned n) { assert(n==2); ++interval; }
static void wined3d_texture_load_location(struct wined3d_texture *t,unsigned sub,struct wined3d_context *c,unsigned loc) { ++load; }
static void do_finish(void) { ++finish; }
static void do_swap(void *dc) { assert(dc==ctx.dc); ++swap; }
static void wined3d_context_gl_submit_command_fence(struct wined3d_context_gl *c) { ++fence; }
static void wined3d_swapchain_gl_rotate(struct wined3d_swapchain *s,struct wined3d_context *c) { ++rotate; }
static void wined3d_texture_validate_location(struct wined3d_texture *t,unsigned sub,unsigned loc) { assert(loc==WINED3D_LOCATION_DRAWABLE); ++validate; }
static void wined3d_texture_invalidate_location(struct wined3d_texture *t,unsigned sub,unsigned loc) { assert(loc==~WINED3D_LOCATION_DRAWABLE); ++invalidate; }
'''
MAIN = r'''
int main(int argc,char **argv) {
    assert(argc==2);
    unsigned count=3; int id=1, expected_gdi=0, invalid=0;
    struct wined3d_pixel_format values[]={{1,WGL_SWAP_COPY_ARB},{2,0},{3,WGL_SWAP_COPY_ARB}};
    RECT src={0,0,640,480},dst={10,10,630,470};
    struct wined3d_texture front={{4}},back={{4}},*backs[]={&back};
    struct wined3d_adapter_gl adapter={NULL,0};
    struct wined3d_device device={&adapter,2,(void *)2};
    struct wined3d_swapchain chain={backs,&front,&device,{{WINED3D_SWAP_EFFECT_COPY,0,640,480}},NULL};
    const struct wined3d_gl_info info={{{do_finish},{do_swap}}};
    ctx.valid=1; ctx.dc=(void *)1; ctx.gl_info=&info;
    if (!strcmp(argv[1],"single-one")) count=1;
    else if (!strcmp(argv[1],"contiguous-last")) id=3;
    else if (!strcmp(argv[1],"sparse-copy")) { values[1].iPixelFormat=7; values[1].swap_method=1; values[2].iPixelFormat=19; id=7; }
    else if (!strcmp(argv[1],"sparse-noncopy")) { values[1].iPixelFormat=7; id=7; expected_gdi=1; }
    else if (!strcmp(argv[1],"single")) { count=1; values[0].iPixelFormat=12; id=12; }
    else if (!strcmp(argv[1],"unknown")) { id=5; expected_gdi=1; }
    else if (!strcmp(argv[1],"zero")) { id=0; expected_gdi=1; }
    else if (!strcmp(argv[1],"empty")) { count=0; expected_gdi=1; }
    else if (!strcmp(argv[1],"full-unknown")) { id=99; dst=src; }
    else if (!strcmp(argv[1],"full-noncopy")) { id=2; dst=src; }
    else if (!strcmp(argv[1],"discard")) { id=99; chain.state.desc.swap_effect=WINED3D_SWAP_EFFECT_DISCARD; }
    else if (!strcmp(argv[1],"backup")) { ctx.dc=device.backup_dc; expected_gdi=1; }
    else if (!strcmp(argv[1],"invalid")) { count=0; ctx.valid=0; invalid=1; }
    else if (!strcmp(argv[1],"windowed-vsync")) { chain.state.desc.windowed=1; chain.state.desc.swap_effect=WINED3D_SWAP_EFFECT_COPY_VSYNC; id=2; expected_gdi=1; }
    adapter.pixel_format_count=count;
    if (count) { adapter.pixel_formats=malloc(count*sizeof(*values)); memcpy(adapter.pixel_formats,values,count*sizeof(*values)); }
    ctx.pixel_format=id; ctx.c.d3d_info=calloc(1,sizeof(*ctx.c.d3d_info)); ctx.c.d3d_info->fences=1;
    swapchain_gl_present(&chain,&src,&dst,2,0);
    assert(acquire==1 && release==1);
    assert(gdi==(unsigned)(!invalid && expected_gdi));
    assert(swap==(unsigned)(!invalid && !expected_gdi));
    assert(blit==swap && interval==swap && load==swap && finish==swap);
    assert(fence==(unsigned)!invalid && rotate==fence && validate==fence && invalidate==fence);
    free(adapter.pixel_formats); free(ctx.c.d3d_info);
    puts("production present branch and lifecycle passed"); return 0;
}
'''


class PixelFormatTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        base.WineShmStateCacheTest.setUpClass.__func__(cls)
        fixed=cls.first_replay['dlls/wined3d/swapchain.c'].decode()
        old=subprocess.check_output(['git','show','HEAD:dlls/wined3d/swapchain.c'],cwd=base.options.wine_src).decode()
        cls.binaries={}
        for name,source in (('old',old),('fixed',fixed)):
            body=base.function(source,'static bool swapchain_present_is_partial_copy(')
            if name=='fixed': body+=base.function(source,'static const struct wined3d_pixel_format *swapchain_gl_find_pixel_format(')
            body+=base.function(source,'static void swapchain_gl_present(')
            c=cls.folder/(name+'-pixel.c'); c.write_text(STUBS+body+MAIN); binary=c.with_suffix('')
            subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-sign-compare',
                            '-fsanitize=address,undefined','-fno-pie','-no-pie',str(c),'-o',str(binary)],check=True)
            cls.binaries[name]=binary

    def run_case(self,case,name='fixed'):
        return subprocess.run([str(self.binaries[name]),case],capture_output=True,text=True,timeout=10,
                              env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0:halt_on_error=1'))

    def test_production_present_matrix(self):
        for case in ('single-one','contiguous-first','contiguous-last','sparse-copy','sparse-noncopy','single','unknown','zero',
                     'empty','full-unknown','full-noncopy','discard','backup','invalid','windowed-vsync'):
            with self.subTest(case=case):
                r=self.run_case(case); self.assertEqual(r.returncode,0,r.stdout+r.stderr)

    def test_original_indexes_outside_compact_array(self):
        for case in ('single-one','contiguous-last','sparse-copy','single'):
            with self.subTest(case=case):
                r=self.run_case(case,'old'); self.assertNotEqual(r.returncode,0); self.assertIn('heap-buffer-overflow',r.stderr)

    def test_overlay_chain_idempotent(self):
        self.assertEqual(self.first_replay,self.second_replay)


if __name__=='__main__':
    unittest.main(argv=[__file__]+base.remaining)
