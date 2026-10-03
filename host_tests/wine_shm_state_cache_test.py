"""Count real Wine Wayland requests after replaying the registered overlays.

Only protocol/platform entry points are stubs. The surface struct and complete
create/destroy/role/reconfigure/SHM functions come from the effective Wine source.
This proves metadata suppression, not compositor rendering or device FPS.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--wine-src', type=Path, default=ROOT / 'thirdparty/wine-valve')
options, remaining = parser.parse_known_args()


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index('\n}\n', start) + 3]


STUB_TYPES = r'''
#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
typedef int BOOL, HRGN;
typedef void *HWND, *HCURSOR, *HANDLE;
typedef wchar_t WCHAR;
typedef int32_t wl_fixed_t;
typedef struct { int left, top, right, bottom; } RECT;
typedef struct { WCHAR *Buffer; unsigned MaximumLength; } UNICODE_STRING;
typedef struct { struct { unsigned nCount; } rdh; RECT Buffer[2]; } RGNDATA;
#define FALSE 0
#define TRUE 1
#define TRACE(...) ((void)0)
#define ERR(...) ((void)0)
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define min(a,b) ((a) < (b) ? (a) : (b))
#define max(a,b) ((a) > (b) ? (a) : (b))
#define WINEHUA_DESKTOP_COORD_MIN -100000
#define WINEHUA_DESKTOP_COORD_MAX 100000
enum wayland_surface_role { WAYLAND_SURFACE_ROLE_NONE,
    WAYLAND_SURFACE_ROLE_TOPLEVEL, WAYLAND_SURFACE_ROLE_SUBSURFACE };
enum wayland_surface_config_state { WAYLAND_SURFACE_CONFIG_STATE_MAXIMIZED=1,
    WAYLAND_SURFACE_CONFIG_STATE_FULLSCREEN=2, WAYLAND_SURFACE_CONFIG_STATE_RESIZING=4,
    WAYLAND_SURFACE_CONFIG_STATE_TILED=8 };
struct wl_surface { int alive; };
struct wp_viewport { int alive; };
struct xdg_surface { int alive; };
struct xdg_toplevel { int alive; };
struct wl_buffer { int alive; };
struct wayland_shm_buffer { struct wl_buffer *wl_buffer; int width, height, busy, ref; };
'''

STUB_CALLS = r'''
static struct wl_surface wl = {1};
static struct wp_viewport viewport = {1};
static struct xdg_surface xdg = {1};
static struct xdg_toplevel toplevel = {1};
static struct wl_buffer buffer = {1};
static int geometry, source, destination, attach, null_attach, commit, damage, flush, ack;
static int source_destroy, xdg_destroy, toplevel_destroy, wl_destroy, fail_role;
static int last_geo[4], last_src[4], last_dst[2];
static RECT monitor_rect = {0, 0, 1920, 1080};
static const char *process_name;
static const WCHAR winehua_window_surface_prop[] = L"surface";
static const int xdg_surface_listener, xdg_toplevel_listener;
static struct {
    struct { pthread_mutex_t mutex; HWND focused_hwnd, constraint_hwnd; int enter_serial; } pointer;
    struct { pthread_mutex_t mutex; HWND focused_hwnd; } keyboard, text_input;
    void *wl_compositor, *wp_viewporter, *xdg_wm_base, *wl_display, *xdg_toplevel_icon_manager_v1;
} process_wayland = {
    .pointer.mutex = PTHREAD_MUTEX_INITIALIZER,
    .keyboard.mutex = PTHREAD_MUTEX_INITIALIZER,
    .text_input.mutex = PTHREAD_MUTEX_INITIALIZER
};
static wl_fixed_t wl_fixed_from_int(int n) { return n * 256; }
static struct wl_surface *wl_compositor_create_surface(void *c) { (void)c; wl.alive=1; return &wl; }
static void wl_surface_set_user_data(struct wl_surface *s, HWND h) { assert(s->alive); (void)h; }
static struct wp_viewport *wp_viewporter_get_viewport(void *v, struct wl_surface *s) {
    (void)v; assert(s->alive); viewport.alive=1; return &viewport;
}
static void winehua_publish_window_surface(HWND h, struct wayland_surface *s) { (void)h; (void)s; }
static void wayland_pointer_clear_constraint(void) {}
static void NtUserSetProp(HWND h, const WCHAR *n, HANDLE v) { (void)h; (void)n; (void)v; }
static void winehua_modal_release(struct wayland_surface *s) { (void)s; }
static void wl_surface_destroy(struct wl_surface *s) { assert(s->alive); s->alive=0; wl_destroy++; }
static void wp_viewport_destroy(struct wp_viewport *v) { assert(v->alive); v->alive=0; source_destroy++; }
static void wl_surface_attach(struct wl_surface *s, struct wl_buffer *b, int x, int y) {
    assert(s && s->alive && x==0 && y==0); if (b) attach++; else null_attach++;
}
static void wl_surface_commit(struct wl_surface *s) { assert(s && s->alive); commit++; }
static void wl_display_flush(void *d) { (void)d; flush++; }
static void xdg_surface_set_window_geometry(struct xdg_surface *s, int x, int y, int w, int h) {
    assert(s && s->alive && w>0 && h>0); geometry++;
    last_geo[0]=x; last_geo[1]=y; last_geo[2]=w; last_geo[3]=h;
}
static void wp_viewport_set_source(struct wp_viewport *v, wl_fixed_t x, wl_fixed_t y, wl_fixed_t w, wl_fixed_t h) {
    assert(v && v->alive); source++; last_src[0]=x; last_src[1]=y; last_src[2]=w; last_src[3]=h;
}
static void wp_viewport_set_destination(struct wp_viewport *v, int w, int h) {
    assert(v && v->alive); destination++; last_dst[0]=w; last_dst[1]=h;
}
static void xdg_surface_ack_configure(struct xdg_surface *s, uint32_t serial) { assert(s && serial); ack++; }
static void xdg_surface_destroy(struct xdg_surface *s) { assert(s->alive); s->alive=0; xdg_destroy++; }
static void xdg_toplevel_destroy(struct xdg_toplevel *s) { assert(s->alive); s->alive=0; toplevel_destroy++; }
static void xdg_toplevel_icon_manager_v1_set_icon(void *m, struct xdg_toplevel *t, void *v) { (void)m; (void)t; (void)v; }
static void xdg_toplevel_icon_v1_destroy(void *v) { (void)v; }
static void wl_subsurface_destroy(void *s) { (void)s; }
static void wayland_shm_buffer_ref(struct wayland_shm_buffer *b) { b->ref++; }
static void wayland_shm_buffer_unref(struct wayland_shm_buffer *b) { b->ref--; }
static RGNDATA *get_region_data(HRGN r) {
    (void)r; RGNDATA *data=calloc(1,sizeof(*data)); data->rdh.nCount=2;
    data->Buffer[0]=(RECT){0,0,10,10}; data->Buffer[1]=(RECT){20,20,25,25}; return data;
}
static void wl_surface_damage_buffer(struct wl_surface *s, int x, int y, int w, int h) {
    assert(s && s->alive && w>0 && h>0); (void)x; (void)y; damage++;
}
static void SetRect(RECT *r, int x, int y, int right, int bottom) { *r=(RECT){x,y,right,bottom}; }
static int IsRectEmpty(const RECT *r) { return r->right<=r->left || r->bottom<=r->top; }
static void wayland_surface_get_rect_in_monitor(struct wayland_surface *s, RECT *r) { (void)s; *r=monitor_rect; }
static void wayland_surface_update_min_max(struct wayland_surface *s) { (void)s; }
static void wayland_surface_reconfigure_subsurface(struct wayland_surface *s) { (void)s; }
static struct xdg_surface *xdg_wm_base_get_xdg_surface(void *b, struct wl_surface *s) {
    (void)b; assert(s && s->alive); xdg.alive=1; return fail_role ? NULL : &xdg;
}
static struct xdg_toplevel *xdg_surface_get_toplevel(struct xdg_surface *s) { assert(s->alive); toplevel.alive=1; return &toplevel; }
static void xdg_surface_add_listener(struct xdg_surface *s, const int *l, HWND h) { (void)s; (void)l; (void)h; }
static void xdg_toplevel_add_listener(struct xdg_toplevel *s, const int *l, HWND h) { (void)s; (void)l; (void)h; }
static int NtUserGetClassName(HWND h, BOOL a, UNICODE_STRING *s) { (void)h; (void)a; (void)s; return 0; }
static void xdg_toplevel_set_app_id(struct xdg_toplevel *t, const char *s) { (void)t; (void)s; }
static int NtUserInternalGetWindowText(HWND h, WCHAR *s, int n) { (void)h; (void)s; (void)n; return 0; }
static void wayland_surface_set_title(struct wayland_surface *s, const WCHAR *t) { (void)s; (void)t; }
static void wayland_surface_assign_icon(struct wayland_surface *s) { (void)s; }
void wayland_surface_clear_role(struct wayland_surface *surface);
void wayland_surface_destroy(struct wayland_surface *surface);
'''

MAIN = r'''
static void reset_counts(void) {
    geometry=source=destination=attach=null_attach=commit=damage=flush=ack=0;
    source_destroy=xdg_destroy=toplevel_destroy=wl_destroy=0;
}
static struct wayland_surface *new_surface(void) {
    struct wayland_surface *s=wayland_surface_create((HWND)1);
    assert(s); wayland_surface_make_toplevel(s);
    s->window.rect=(RECT){10,20,650,500}; s->window.scale=1.0;
    s->requested=(struct wayland_surface_config){640,480,0,1,0};
    return s;
}
static void frame(struct wayland_surface *s, int w, int h) {
    struct wayland_shm_buffer b={&buffer,w,h,0,0};
    assert(wayland_surface_reconfigure(s));
    wayland_surface_attach_shm(s,&b,1);
    wl_surface_commit(s->wl_surface);
    assert(b.busy && b.ref==1 && s->has_contents);
}
int main(int argc, char **argv) {
    assert(argc==2); const char *mode=argv[1];
    struct wayland_surface *s=new_surface(); reset_counts();
    if (!strcmp(mode,"steady")) {
        for (int i=0;i<240;i++) frame(s,640,480);
        assert(geometry==1 && source==1 && destination==1);
        assert(attach==240 && damage==480 && commit==240 && ack==1);
        assert(s->content_width==640 && s->content_height==480);
    } else if (!strcmp(mode,"resize")) {
        frame(s,640,480); s->window.rect.right=810; s->window.rect.bottom=620;
        frame(s,800,600); frame(s,800,600);
        assert(geometry==2 && source==2 && destination==2);
        assert(last_geo[2]==800 && last_geo[3]==600 && last_src[2]==800*256);
        frame(s,320,200); frame(s,320,200);
        assert(source==3 && last_src[2]==320*256 && last_src[3]==200*256);
        assert(s->content_width==800 && s->content_height==600 && attach==5 && commit==5);
    } else if (!strcmp(mode,"scale")) {
        frame(s,640,480); s->window.scale=2.0; frame(s,640,480); frame(s,640,480);
        assert(geometry==2 && source==2 && destination==2 && last_dst[0]==320 && last_dst[1]==240);
    } else if (!strcmp(mode,"configure")) {
        frame(s,640,480);
        s->window.state=WAYLAND_SURFACE_CONFIG_STATE_FULLSCREEN;
        s->processing=(struct wayland_surface_config){640,480,s->window.state,2,TRUE};
        frame(s,640,480); frame(s,640,480);
        assert(ack==2 && geometry==2 && source==2 && destination==2);
        s->window.state=WAYLAND_SURFACE_CONFIG_STATE_MAXIMIZED;
        s->processing=(struct wayland_surface_config){800,600,s->window.state,3,TRUE};
        assert(!wayland_surface_reconfigure(s));
        assert(ack==2 && attach==3 && commit==3); /* never submit an incompatible frame */
        s->window.rect.right=810; s->window.rect.bottom=620;
        frame(s,800,600); assert(ack==3 && geometry==3 && source==3 && destination==3);
    } else if (!strcmp(mode,"role")) {
        frame(s,640,480); wayland_surface_make_toplevel(s); frame(s,640,480);
        assert(geometry==1 && source==1 && destination==1); /* existing role is stable */
        wayland_surface_clear_role(s);
        assert(!s->xdg_surface && !s->has_contents && null_attach==1 && xdg_destroy==1);
        wayland_surface_make_toplevel(s); s->requested=(struct wayland_surface_config){640,480,0,2,0};
        frame(s,640,480); frame(s,640,480);
        assert(geometry==2 && source==2 && destination==2 && ack==2 && commit==7);
    } else if (!strcmp(mode,"viewport")) {
        frame(s,640,480);
        wayland_surface_destroy_viewport(s);
        assert(!s->wp_viewport && source_destroy==1);
        frame(s,640,480); /* no requests to a null viewport; attach/commit remain intact */
        assert(source==1 && destination==1 && attach==2 && commit==2);
        s->wp_viewport=wp_viewporter_get_viewport(NULL,s->wl_surface);
        frame(s,640,480); frame(s,640,480);
        assert(source==2 && destination==2 && geometry==1 && attach==4 && commit==4);
    } else if (!strcmp(mode,"reset")) {
        frame(s,640,480);
        wayland_surface_reconfigure_size(s,0,480); wayland_surface_reconfigure_size(s,0,480);
        assert(destination==2 && last_dst[0]==-1 && last_dst[1]==-1);
        wayland_surface_reconfigure_size(s,640,480);
        assert(destination==3 && last_dst[0]==640);
        s->window.rect.right=s->window.rect.left; s->window.rect.bottom=s->window.rect.top;
        frame(s,640,480); frame(s,640,480);
        assert(last_src[2]==256 && last_src[3]==256 && geometry==1 && source==2 && destination==4);
    } else if (!strcmp(mode,"desktop")) {
        setenv("WINEHUA_DESKTOP_MODE","1",1); frame(s,640,480);
        assert(last_geo[0]==10 && last_geo[1]==20);
        s->window.rect=(RECT){30,40,670,520}; frame(s,640,480); frame(s,640,480);
        assert(geometry==2 && source==1 && destination==1 && last_geo[0]==30 && last_geo[1]==40);
        unsetenv("WINEHUA_DESKTOP_MODE"); frame(s,640,480);
        assert(geometry==3 && last_geo[0]==0 && last_geo[1]==0);
    } else if (!strcmp(mode,"recreate")) {
        frame(s,640,480); wayland_surface_destroy(s);
        assert(source_destroy==1 && xdg_destroy==1 && toplevel_destroy==1 && wl_destroy==1);
        s=new_surface(); frame(s,640,480); frame(s,640,480);
        assert(geometry==2 && source==2 && destination==2);
    } else if (!strcmp(mode,"role-failure")) {
        frame(s,640,480); wayland_surface_clear_role(s); fail_role=1;
        wayland_surface_make_toplevel(s); assert(!s->xdg_surface); fail_role=0;
        wayland_surface_make_toplevel(s); s->requested=(struct wayland_surface_config){640,480,0,2,0};
        frame(s,640,480); assert(geometry==2 && source==2 && destination==2);
    } else assert(0);
    wayland_surface_destroy(s); puts(mode); return 0;
}
'''

HOST_STUBS = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
using wl_fixed_t = int32_t;
struct wl_client {};
struct wl_resource { void *data; };
struct ViewportState { double x=0, y=0, width=-1, height=-1; int destinationW=-1, destinationH=-1; };
struct CommittedSurface {
    enum class Role { None, Toplevel, Subsurface };
    bool hasWindowGeometry=false; Role role=Role::Toplevel;
    struct { int x=0, y=0, w=0, h=0; } contentRect;
};
struct SurfaceData {
    CommittedSurface committed;
    ViewportState directViewportPending;
    int vpSrcX=0, vpSrcY=0, vpSrcW=-1, vpSrcH=-1, vpDstW=-1, vpDstH=-1;
    unsigned toplevelId=1;
};
struct ViewportResource { wl_resource *surface; };
struct XdgSurface { wl_resource *wlSurface; };
struct ShmCommitInfo {
    int bufW=640, bufH=480, stride=2560, contentOffX=0, contentOffY=0;
    int contentW=640, contentH=480, screenX=0, screenY=0;
};
static int logs, warnings, errors, destroyed, geometry_calculations;
static bool trace_enabled;
#define LOG_APP 0
#define WP_VIEWPORT_ERROR_BAD_VALUE 1
#define OH_LOG_INFO(...) (++logs)
#define OH_LOG_WARN(...) (++warnings)
namespace winehua { static bool FrameTraceEnabled() { return trace_enabled; } }
static void *wl_resource_get_user_data(wl_resource *r) { return r->data; }
static void wl_resource_post_error(wl_resource*, int, const char*) { errors++; }
static void wl_resource_destroy(wl_resource*) { destroyed++; }
static wl_fixed_t wl_fixed_from_int(int value) { return value*256; }
static double wl_fixed_to_double(wl_fixed_t value) { return value/256.0; }
static int wl_fixed_to_int(wl_fixed_t value) { return value/256; }
static wl_resource *ViewportSurface(wl_resource *r) { return static_cast<ViewportResource*>(r->data)->surface; }
static void ComputeContentAreaGeometry(ShmCommitInfo&, bool, int, int, bool, int, int) { geometry_calculations++; }
class WaylandServer {
public:
    static void viewport_set_source(wl_client*, wl_resource*, wl_fixed_t, wl_fixed_t, wl_fixed_t, wl_fixed_t);
    static void viewport_set_destination(wl_client*, wl_resource*, int32_t, int32_t);
    static void viewport_destroy(wl_client*, wl_resource*);
    void ComputeContentArea(SurfaceData*, ShmCommitInfo&);
};
'''

HOST_MAIN = r'''
int main() {
    SurfaceData sd; wl_resource surface{&sd}; ViewportResource vr{&surface}; wl_resource vp{&vr};
    XdgSurface xs{&surface}; wl_resource xdg{&xs};
    for (int i=0;i<240;i++) {
        WaylandServer::viewport_set_source(nullptr,&vp,0,0,640*256,480*256);
        WaylandServer::viewport_set_destination(nullptr,&vp,640,480);
        xs_set_window_geometry(nullptr,&xdg,0,0,640,480);
    }
    assert(logs==2 && errors==0);
    assert(sd.directViewportPending.width==640 && sd.vpDstW==640 && sd.committed.contentRect.w==640);
    /* Fractional protocol changes count even when the legacy integer field is unchanged. */
    WaylandServer::viewport_set_source(nullptr,&vp,0,0,640*256+1,480*256);
    assert(logs==3 && sd.directViewportPending.width==640+1/256.0);
    WaylandServer::viewport_set_source(nullptr,&vp,-1,-1,0,0);
    WaylandServer::viewport_set_destination(nullptr,&vp,0,0);
    assert(errors==2 && logs==3 && sd.vpDstW==640);
    WaylandServer::viewport_set_source(nullptr,&vp,-256,-256,-256,-256);
    assert(sd.directViewportPending.width==-1 && sd.vpSrcW==-1);
    WaylandServer::viewport_set_source(nullptr,&vp,0,0,640*256,480*256); assert(logs==4);
    WaylandServer::viewport_destroy(nullptr,&vp);
    assert(destroyed==1 && sd.directViewportPending.width==-1 && sd.directViewportPending.destinationW==-1);
    WaylandServer::viewport_set_source(nullptr,&vp,0,0,640*256,480*256); assert(logs==5);
    WaylandServer host; ShmCommitInfo fi;
    for (int i=0;i<240;i++) host.ComputeContentArea(&sd,fi);
    assert(logs==5 && warnings==0 && geometry_calculations==240);
    trace_enabled=true; host.ComputeContentArea(&sd,fi); assert(logs==6 && geometry_calculations==241);
    trace_enabled=false; fi.stride=2564; host.ComputeContentArea(&sd,fi);
    assert(logs==6 && warnings==1 && geometry_calculations==242);
    vr.surface=nullptr;
    WaylandServer::viewport_set_source(nullptr,&vp,0,0,640*256,480*256);
    assert(logs==6); puts("host: change-only metadata and opt-in frame logs passed");
}
'''


class WineShmStateCacheTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix='wine-shm-state-cache-')
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.folder = Path(cls.temporary.name)
        script = (ROOT / 'scripts/build_wine.sh').read_text()
        helper = function(script, 'ensure_wine_patch() {')
        patches = [ROOT / 'patches/wine' / name for name in re.findall(
            r'ensure_wine_patch "\$SCRIPT_DIR/\.\./patches/wine/([^"]+)"', script)]
        relatives = {'dlls/winewayland.drv/wayland_surface.c', 'dlls/winewayland.drv/waylanddrv.h'}
        added = set()
        for patch in patches:
            relatives.update(re.findall(r'^\+\+\+ b/(.+)$', patch.read_text(), re.M))
            added.update(re.findall(r'^--- /dev/null\n\+\+\+ b/(.+)$', patch.read_text(), re.M))
        tree = cls.folder / 'wine'
        for relative in relatives - added:
            path = tree / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(subprocess.check_output(['git', 'show', 'HEAD:' + relative], cwd=options.wine_src))
        command = ('set -euo pipefail\nlog() { :; }\n' + helper +
                   '\nWINE_SRC="$1"\nshift\nfor patch_file in "$@"; do ensure_wine_patch "$patch_file" test; done\n')
        replay = ['bash', '-c', command, 'patch-replay', str(tree), *map(str, patches)]
        subprocess.run(replay, check=True, capture_output=True, text=True)
        cls.first_replay = {r: (tree / r).read_bytes() for r in relatives}
        subprocess.run(replay, check=True, capture_output=True, text=True)
        cls.second_replay = {r: (tree / r).read_bytes() for r in relatives}
        header = (tree / 'dlls/winewayland.drv/waylanddrv.h').read_text()
        source = (tree / 'dlls/winewayland.drv/wayland_surface.c').read_text()
        structs = ''
        for name in ('wayland_surface_config', 'wayland_window_config', 'wayland_surface'):
            start = header.index('struct ' + name + '\n{')
            structs += header[start:header.index('\n};', start) + 4] + '\n'
        functions = ''
        for signature in ('static void wayland_surface_invalidate_state_cache(',
                          'static void wayland_surface_refresh_state_cache(',
                          'static void wayland_surface_destroy_viewport('):
            if signature in source:
                functions += function(source, signature)
        # Baseline fallback lets the steady test demonstrate the original
        # repeated requests before the new lifecycle helper exists.
        if 'static void wayland_surface_destroy_viewport(' not in source:
            functions += '''static void wayland_surface_destroy_viewport(struct wayland_surface *s) {
                if (s->wp_viewport) wp_viewport_destroy(s->wp_viewport);
                s->wp_viewport=NULL;
            }\n'''
        for signature in ('void wayland_surface_coords_from_window(',
                          'BOOL wayland_surface_config_is_compatible(',
                          'static void wayland_surface_reconfigure_geometry(',
                          'static void wayland_surface_reconfigure_size(',
                          'static BOOL wayland_surface_reconfigure_xdg(',
                          'BOOL wayland_surface_reconfigure(',
                          'void wayland_surface_clear_role(',
                          'struct wayland_surface *wayland_surface_create(',
                          'void wayland_surface_make_toplevel(',
                          'void wayland_surface_destroy(',
                          'void wayland_surface_attach_shm('):
            functions += function(source, signature)
        cfile = cls.folder / 'requests.c'
        cfile.write_text(STUB_TYPES + structs + STUB_CALLS + functions + MAIN)
        cls.binary = cls.folder / 'requests'
        subprocess.run(['gcc', '-D_GNU_SOURCE', '-D__OHOS__', '-std=c11', '-Wall', '-Wextra',
                        '-Werror', '-Wno-unused-variable', '-Wno-unused-parameter',
                        '-Wno-unused-function', '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                        '-fno-omit-frame-pointer', '-g', str(cfile), '-lm', '-pthread',
                        '-o', str(cls.binary)], check=True)

    def run_case(self, mode):
        # LSan requires ptrace, unavailable in some managed host-test runners.
        # ASan/UBSan memory-access checks remain on; opt in to LSan when supported.
        leaks = os.environ.get('WINEHUA_TEST_ASAN_LEAKS', '0')
        environment = dict(os.environ, ASAN_OPTIONS=f'detect_leaks={leaks}:halt_on_error=1')
        environment.pop('WINEHUA_WINDOW_TOKEN', None)
        environment.pop('WINEHUA_DESKTOP_MODE', None)
        result = subprocess.run([str(self.binary), mode], capture_output=True, text=True,
                                env=environment, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_registered_overlay_replays_idempotently(self):
        self.assertEqual(self.first_replay, self.second_replay)

    def test_240_frames_keep_all_attach_damage_commit_and_one_metadata_set(self): self.run_case('steady')
    def test_resize_and_buffer_source_clamp(self): self.run_case('resize')
    def test_scale_change_resends_same_source(self): self.run_case('scale')
    def test_configure_fullscreen_and_incompatible_resize(self): self.run_case('configure')
    def test_role_unmap_remap_and_noop_role_creation(self): self.run_case('role')
    def test_viewport_destroy_null_and_recreate(self): self.run_case('viewport')
    def test_destination_unset_restore_and_zero_size(self): self.run_case('reset')
    def test_desktop_position_and_standalone_geometry(self): self.run_case('desktop')
    def test_window_surface_destroy_and_recreate(self): self.run_case('recreate')
    def test_failed_role_creation_and_retry(self): self.run_case('role-failure')


class HostShmLogPolicyTest(unittest.TestCase):
    def test_production_metadata_logs_and_frame_trace_gate(self):
        source = (ROOT / 'entry/src/main/cpp/compositor/wl_core.cpp').read_text()
        xdg = (ROOT / 'entry/src/main/cpp/compositor/xdg_shell.cpp').read_text()
        functions = ''.join(function(source, signature) for signature in (
            'void WaylandServer::viewport_set_source(', 'void WaylandServer::viewport_set_destination(',
            'void WaylandServer::viewport_destroy(', 'void WaylandServer::ComputeContentArea('))
        functions += function(xdg, 'static void xs_set_window_geometry(')
        with tempfile.TemporaryDirectory(prefix='host-shm-log-policy-') as temporary:
            path = Path(temporary)
            (path / 'logs.cpp').write_text(HOST_STUBS + functions + HOST_MAIN)
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            str(path / 'logs.cpp'), '-o', str(path / 'logs')], check=True)
            result = subprocess.run([str(path / 'logs')], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_both_commit_logs_are_inside_opt_in_guards(self):
        source = (ROOT / 'entry/src/main/cpp/compositor/wl_core.cpp').read_text()
        toplevel = function(source, 'void WaylandServer::UpdateToplevelFrameOnCommit(')
        surface = function(source, 'void WaylandServer::surface_commit(')
        self.assertRegex(toplevel, r'if \(winehua::FrameTraceEnabled\(\)\) \{\s*OH_LOG_INFO\(LOG_APP, "\[MW-COMMIT\] toplevel')
        self.assertRegex(surface, r'if \(frameTrace\) \{\s*OH_LOG_INFO\(LOG_APP, "\[MW-COMMIT\] surface')
        self.assertEqual(toplevel.count('"[MW-COMMIT]'), 1)
        self.assertEqual(surface.count('"[MW-COMMIT]'), 1)


if __name__ == '__main__':
    unittest.main(argv=[__file__] + remaining)
