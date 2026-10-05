"""Replay the production client-only parent lifecycle across hide/show.

Protocol and Win32 queries are boundaries; role creation and WindowPosChanged
come from the registered Wine overlays. This does not prove game rendering.
"""
import os
import subprocess
import unittest

import wine_shm_state_cache_test as base

STUBS = r'''
typedef unsigned DWORD, UINT;
#define WS_VISIBLE 0x10000000u
#define WS_EX_LAYERED 0x00080000u
#define WS_EX_TRANSPARENT 0x20u
#define GWL_STYLE -16
#define GWL_EXSTYLE -20
#define GA_ROOT 2
#define WINE_SWP_FULLSCREEN 0x10000u
struct window_rects { RECT window, client; };
struct window_surface { int unused; };
struct wl_region { int unused; };
struct wayland_client_surface { HWND toplevel; struct wayland_surface *parent; };
struct wayland_win_data {
    HWND hwnd;
    struct wayland_surface *wayland_surface;
    struct wayland_client_surface *client_surface;
    struct window_rects rects;
    BOOL is_fullscreen, managed, layered_attribs_set;
};
static struct wayland_win_data wd, parent_wd;
static HWND root;
static unsigned style, exstyle;
static int lock_held, updates;
static BOOL managed=TRUE;
static DWORD NtUserGetWindowLongW(HWND h, int index) {
    assert(h==wd.hwnd); return index==GWL_STYLE ? style : exstyle;
}
static BOOL NtUserIsWindowVisible(HWND h) { assert(h==wd.hwnd); return !!(style&WS_VISIBLE); }
static HWND NtUserGetAncestor(HWND h, int flag) { assert(h==wd.hwnd && flag==GA_ROOT); return root; }
static HWND NtUserGetForegroundWindow(void) { return NULL; }
static BOOL is_window_managed(HWND h, UINT flags, BOOL fs) { assert(h==wd.hwnd && !lock_held); return managed; }
static struct wayland_win_data *wayland_win_data_get_nolock(HWND h) {
    assert(lock_held); return h==wd.hwnd ? &wd : h==parent_wd.hwnd ? &parent_wd : NULL;
}
static struct wayland_win_data *wayland_win_data_get(HWND h) {
    assert(h==wd.hwnd && !lock_held); lock_held=1; return &wd;
}
static void wayland_win_data_release(struct wayland_win_data *d) { assert(d==&wd && lock_held); lock_held=0; }
static struct wl_region region;
static struct wl_region *wl_compositor_create_region(void *c) { return &region; }
static void wl_surface_set_input_region(struct wl_surface *s, struct wl_region *r) { assert(s && s->alive); }
static void wl_region_destroy(struct wl_region *r) { assert(r==&region); }
static void reapply_cursor_clipping(void) {}
static void wayland_win_data_get_config(struct wayland_win_data *d, struct wayland_window_config *c) {
    c->visible=!!(style&WS_VISIBLE); c->rect=d->rects.window; c->managed=d->managed;
}
static void wayland_win_data_update_wayland_state(struct wayland_win_data *d) {
    assert(d->wayland_surface); updates++;
}
static void winehua_modal_update(struct wayland_surface *s) { assert(s); }
static void wayland_surface_make_subsurface(struct wayland_surface *s, struct wayland_surface *p) {
    assert(p); s->role=WAYLAND_SURFACE_ROLE_SUBSURFACE;
}
static void wayland_client_surface_attach(struct wayland_client_surface *c, HWND h) {
    assert(lock_held);
    struct wayland_win_data *d = h ? wayland_win_data_get_nolock(h) : NULL;
    c->parent=d ? d->wayland_surface : NULL;
    c->toplevel=c->parent ? h : NULL;
}
'''

MAIN = r'''
static void reset(void) {
    if (wd.wayland_surface) wayland_surface_destroy(wd.wayland_surface);
    memset(&wd,0,sizeof(wd)); memset(&parent_wd,0,sizeof(parent_wd));
    wd.hwnd=(HWND)1; root=wd.hwnd; style=0; exstyle=0; managed=TRUE;
    lock_held=0; updates=0;
}
static void change(struct window_surface *gdi) {
    const struct window_rects rects={{0,0,1200,800},{0,0,1200,800}};
    WAYLAND_WindowPosChanged(wd.hwnd,NULL,NULL,WINE_SWP_FULLSCREEN,&rects,gdi);
    assert(!lock_held);
}
int main(int argc, char **argv) {
    struct window_surface gdi={0}; struct wayland_client_surface client={0};
    reset(); wd.client_surface=&client;
    if (!strcmp(argv[1],"hidden-show")) {
        change(&gdi); assert(wd.wayland_surface && !wd.wayland_surface->xdg_surface);
        style=WS_VISIBLE; change(NULL);
        assert(wd.wayland_surface && wd.wayland_surface->xdg_surface);
        assert(client.parent==wd.wayland_surface && client.toplevel==wd.hwnd);
        int destroys=toplevel_destroy;
        for (int i=0;i<240;i++) change(NULL);
        assert(toplevel_destroy==destroys && wd.wayland_surface->xdg_surface);
    } else if (!strcmp(argv[1],"destroy-show")) {
        style=WS_VISIBLE; change(&gdi); assert(wd.wayland_surface->xdg_surface);
        style=0; change(NULL); assert(!wd.wayland_surface && !client.parent);
        style=WS_VISIBLE; change(NULL);
        assert(wd.wayland_surface && wd.wayland_surface->xdg_surface);
        assert(client.parent==wd.wayland_surface);
    } else if (!strcmp(argv[1],"layered")) {
        exstyle=WS_EX_LAYERED; style=WS_VISIBLE; change(&gdi);
        assert(wd.wayland_surface && !wd.wayland_surface->xdg_surface);
        change(NULL); assert(!wd.wayland_surface->xdg_surface);
        wd.layered_attribs_set=TRUE; change(NULL); assert(wd.wayland_surface->xdg_surface);
    } else if (!strcmp(argv[1],"child")) {
        parent_wd.hwnd=(HWND)2; root=parent_wd.hwnd;
        parent_wd.wayland_surface=wayland_surface_create(root);
        style=WS_VISIBLE; change(NULL);
        assert(!wd.wayland_surface && client.parent==parent_wd.wayland_surface);
        style=0; change(NULL); assert(!client.parent);
        wayland_surface_destroy(parent_wd.wayland_surface); parent_wd.wayland_surface=NULL;
    } else if (!strcmp(argv[1],"no-client")) {
        wd.client_surface=NULL; style=WS_VISIBLE; change(NULL); assert(!wd.wayland_surface);
        change(&gdi); assert(wd.wayland_surface->xdg_surface);
        style=0; change(NULL); assert(!wd.wayland_surface);
    } else if (!strcmp(argv[1],"role-failure")) {
        style=WS_VISIBLE; fail_role=1; change(NULL);
        assert(wd.wayland_surface && !wd.wayland_surface->xdg_surface);
        fail_role=0; change(NULL); assert(wd.wayland_surface->xdg_surface);
        assert(client.parent==wd.wayland_surface);
    } else abort();
    reset(); puts("client-only parent lifecycle PASS"); return 0;
}
'''


class ClientRemapTest(base.WineShmStateCacheTest):
    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        prefix = 'dlls/winewayland.drv/'
        header = cls.first_replay[prefix+'waylanddrv.h'].decode()
        window = cls.first_replay[prefix+'window.c'].decode()
        surface = cls.first_replay[prefix+'wayland_surface.c'].decode()
        structs = ''
        for name in ('wayland_surface_config', 'wayland_window_config', 'wayland_surface'):
            start = header.index('struct '+name+'\n{')
            structs += header[start:header.index('\n};',start)+4]+'\n'
        functions = ''.join(base.function(surface, sig) for sig in (
            'static void wayland_surface_invalidate_state_cache(',
            'static void wayland_surface_destroy_viewport(',
            'void wayland_surface_clear_role(',
            'struct wayland_surface *wayland_surface_create(',
            'void wayland_surface_make_toplevel(', 'void wayland_surface_destroy('))
        functions += base.function(window, 'static BOOL wayland_win_data_create_wayland_surface(')
        functions += base.function(window, 'void WAYLAND_WindowPosChanged(')
        path = cls.folder/'client-remap.c'
        path.write_text(base.STUB_TYPES+structs+base.STUB_CALLS+STUBS+functions+MAIN)
        cls.remap_binary = cls.folder/'client-remap'
        subprocess.run(['gcc','-D_GNU_SOURCE','-D__OHOS__','-std=c11','-Wall','-Wextra','-Werror',
            '-Wno-unused-variable','-Wno-unused-parameter','-Wno-unused-function',
            '-fsanitize=address,undefined','-fno-pie','-no-pie','-g',str(path),'-lm','-pthread',
            '-o',str(cls.remap_binary)],check=True)

    def test_client_parent_lifecycle(self):
        for scenario in ('hidden-show','destroy-show','layered','child','no-client','role-failure'):
            with self.subTest(scenario=scenario):
                result = subprocess.run([str(self.remap_binary),scenario],text=True,capture_output=True,
                    timeout=10,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0:halt_on_error=1'))
                self.assertEqual(result.returncode,0,result.stdout+result.stderr)


if __name__ == '__main__':
    unittest.main(argv=[__file__]+base.remaining)
