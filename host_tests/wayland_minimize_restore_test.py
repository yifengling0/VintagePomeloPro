"""Run effective Wine window-state functions against protocol/Win32 boundaries.

The registered overlays and real structs/functions are reused from the SHM
harness. This checks minimize/restore requests and lock reentry, not device
rendering or whether a particular game voluntarily minimizes itself.
"""
import os
from pathlib import Path
import subprocess
import unittest

import wine_shm_state_cache_test as base

STUBS = r'''
typedef unsigned DWORD, UINT;
typedef int INT;
#define WS_MINIMIZE 0x20000000u
#define WS_MAXIMIZE 0x01000000u
#define WS_CAPTION 0x00c00000u
#define WS_VISIBLE 0x10000000u
#define GWL_STYLE -16
#define SWP_FRAMECHANGED 0x0020u
#define SWP_NOSIZE 0x0001u
#define SWP_NOMOVE 0x0002u
#define SWP_NOZORDER 0x0004u
#define SWP_NOACTIVATE 0x0010u
#define SWP_NOOWNERZORDER 0x0200u
#define SWP_NOSENDCHANGING 0x0400u
#define WM_SYSCOMMAND 0x0112u
#define WM_ENTERSIZEMOVE 0x0231u
#define WM_EXITSIZEMOVE 0x0232u
#define SC_RESTORE 0xf120u
struct wayland_win_data {
    HWND hwnd;
    struct wayland_surface *wayland_surface;
    struct { RECT window, client; } rects;
    BOOL is_fullscreen, managed;
};
static struct wayland_win_data wd;
static unsigned style;
static int lock_held, minimize_calls, restore_calls, raw_position_calls;
static int unset_fullscreen_calls, set_fullscreen_calls;
static void wayland_win_data_get_config(struct wayland_win_data*, struct wayland_window_config*);
static void wayland_surface_update_state_toplevel(struct wayland_surface*);
static DWORD NtUserGetWindowLongW(HWND h, int index) { assert(h==wd.hwnd && index==GWL_STYLE); return style; }
static UINT NtUserGetSystemDpiForProcess(UINT p) { (void)p; return 96; }
static struct wayland_win_data *wayland_win_data_get(HWND h) {
    assert(h==wd.hwnd && !lock_held); lock_held=1; return &wd;
}
static void wayland_win_data_release(struct wayland_win_data *d) { assert(d==&wd && lock_held); lock_held=0; }
static BOOL wayland_surface_is_toplevel(struct wayland_surface *s) { return s->role==WAYLAND_SURFACE_ROLE_TOPLEVEL && s->xdg_surface; }
static void xdg_toplevel_set_minimized(struct xdg_toplevel *s) { assert(s && s->alive); minimize_calls++; }
static void xdg_toplevel_unset_fullscreen(struct xdg_toplevel *s) { assert(s && s->alive); unset_fullscreen_calls++; }
static void xdg_toplevel_set_fullscreen(struct xdg_toplevel *s, void *o) { assert(s && s->alive); (void)o; set_fullscreen_calls++; }
static void xdg_toplevel_set_maximized(struct xdg_toplevel *s) { assert(s && s->alive); }
static void xdg_toplevel_unset_maximized(struct xdg_toplevel *s) { assert(s && s->alive); }
static void OffsetRect(RECT *r, int x, int y) { r->left+=x; r->right+=x; r->top+=y; r->bottom+=y; }
static void wayland_surface_coords_from_window(struct wayland_surface *s, int w, int h, int *outw, int *outh) { (void)s; *outw=w; *outh=h; }
static void wayland_surface_coords_to_window(struct wayland_surface *s, int w, int h, int *outw, int *outh) { (void)s; *outw=w; *outh=h; }
static BOOL wayland_surface_config_is_compatible(struct wayland_surface_config *c, int w, int h, unsigned state) { return c->width==w && c->height==h && c->state==state; }
static void NtUserSetWindowLong(HWND h, int index, DWORD v, BOOL b) { (void)h; (void)index; (void)b; style=v; }
static void NtUserSetRawWindowPos(HWND h, RECT r, UINT flags, BOOL b) { (void)h; (void)r; (void)flags; (void)b; assert(!lock_held); raw_position_calls++; }
static void send_message(HWND h, unsigned msg, unsigned wp, unsigned lp) {
    (void)lp; assert(h==wd.hwnd && !lock_held);
    if (msg==WM_SYSCOMMAND) {
        assert(wp==SC_RESTORE); restore_calls++;
        style &= ~WS_MINIMIZE;
        wd.rects.window=(RECT){0,0,1200,800};
        wd.rects.client=wd.rects.window;
        struct wayland_win_data *d=wayland_win_data_get(h);
        wayland_win_data_get_config(d,&d->wayland_surface->window);
        wayland_surface_update_state_toplevel(d->wayland_surface);
        wayland_win_data_release(d);
    }
}
'''

MAIN = r'''
int main() {
    struct wayland_surface surface={0};
    surface.hwnd=(HWND)1; surface.role=WAYLAND_SURFACE_ROLE_TOPLEVEL;
    surface.xdg_surface=&xdg; surface.xdg_toplevel=&toplevel; surface.wl_surface=&wl;
    surface.current=(struct wayland_surface_config){1200,800,WAYLAND_SURFACE_CONFIG_STATE_FULLSCREEN,10,TRUE};
    wd=(struct wayland_win_data){.hwnd=surface.hwnd,.wayland_surface=&surface,.is_fullscreen=TRUE,.managed=TRUE};
    wd.rects.window=(RECT){-32000,-32000,-31840,-31969};
    wd.rects.client=wd.rects.window;
    style=WS_VISIBLE|WS_MINIMIZE;
    wayland_win_data_get_config(&wd,&surface.window);
    assert(surface.window.minimized);
    surface.processing=(struct wayland_surface_config){1200,800,WAYLAND_SURFACE_CONFIG_STATE_FULLSCREEN,11,FALSE};
    for (int i=0;i<240;i++) wayland_surface_update_state_toplevel(&surface);
    assert(minimize_calls==1 && unset_fullscreen_calls==0 && surface.processing.processed);
    // An outstanding resize must be acknowledged without restoring or moving.
    surface.requested=(struct wayland_surface_config){1200,800,WAYLAND_SURFACE_CONFIG_STATE_FULLSCREEN,12,FALSE};
    wayland_configure_window(wd.hwnd);
    assert(ack==1 && restore_calls==0 && raw_position_calls==0 && surface.current.serial==12);
    assert(surface.window.rect.left==-32000 && surface.window.minimized && !lock_held);
    // Explicit compositor restore acknowledges first, then reenters Win32 unlocked.
    surface.requested=(struct wayland_surface_config){0,0,WAYLAND_SURFACE_CONFIG_STATE_FULLSCREEN,13,FALSE};
    wayland_configure_window(wd.hwnd);
    assert(ack==2 && restore_calls==1 && !(style&WS_MINIMIZE) && !surface.minimize_requested);
    assert(!surface.window.minimized && surface.window.rect.right==1200 && !lock_held);
    for (int i=0;i<240;i++) wayland_surface_update_state_toplevel(&surface);
    assert(minimize_calls==1 && unset_fullscreen_calls==0);
    // Ordinary windowed configuration still goes through the normal position path.
    wd.is_fullscreen=FALSE; wayland_win_data_get_config(&wd,&surface.window);
    surface.requested=(struct wayland_surface_config){640,480,0,14,FALSE};
    wayland_configure_window(wd.hwnd);
    assert(raw_position_calls==1 && restore_calls==1 && !lock_held);
    // An initial 0x0 configure of an already minimized window is not a restore.
    style=WS_VISIBLE|WS_MINIMIZE; wayland_win_data_get_config(&wd,&surface.window);
    surface.current.serial=0; wayland_surface_update_state_toplevel(&surface);
    surface.requested=(struct wayland_surface_config){0,0,0,15,FALSE};
    wayland_configure_window(wd.hwnd);
    assert(restore_calls==1 && surface.window.minimized && !lock_held);
    wayland_surface_clear_role(&surface);
    assert(!surface.minimize_requested);
    puts("Wayland minimize/restore production replay PASS");
}
'''


class WaylandMinimizeRestoreTest(base.WineShmStateCacheTest):
    @classmethod
    def setUpClass(cls):
        try:
            super().setUpClass()
        except subprocess.CalledProcessError as error:
            raise RuntimeError((error.stdout or '')+(error.stderr or '')) from error
        prefix = 'dlls/winewayland.drv/'
        header = cls.first_replay[prefix+'waylanddrv.h'].decode()
        window = cls.first_replay[prefix+'window.c'].decode()
        source = cls.first_replay[prefix+'wayland_surface.c'].decode()
        structs = ''
        for name in ('wayland_surface_config', 'wayland_window_config', 'wayland_surface'):
            start = header.index('struct '+name+'\n{')
            structs += header[start:header.index('\n};',start)+4]+'\n'
        functions = ''.join(base.function(window,sig) for sig in (
            'static void wayland_win_data_get_config(', 'static void wayland_surface_update_state_toplevel(',
            'static void wayland_configure_window('))
        lifecycle = base.function(source,'static void wayland_surface_invalidate_state_cache(')
        lifecycle += base.function(source,'void wayland_surface_clear_role(')
        path = cls.folder/'minimize.c'
        path.write_text(base.STUB_TYPES+structs+base.STUB_CALLS+STUBS+lifecycle+functions+MAIN)
        cls.minimize_binary=cls.folder/'minimize'
        subprocess.run(['gcc','-D_GNU_SOURCE','-std=c11','-Wall','-Wextra','-Werror',
            '-Wno-unused-variable','-Wno-unused-parameter','-Wno-unused-function',
            '-fsanitize=address,undefined','-fno-pie','-no-pie','-g',str(path),'-lm','-pthread',
            '-o',str(cls.minimize_binary)],check=True)

    def test_win32_minimize_pending_configure_explicit_restore_and_role_reset(self):
        result=subprocess.run([str(self.minimize_binary)],text=True,capture_output=True,timeout=10,
            env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0:halt_on_error=1'))
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)


if __name__=='__main__':
    unittest.main(argv=[__file__]+base.remaining)
