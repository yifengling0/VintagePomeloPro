"""Exercise production Direct-owner lease helpers at the HWND/role boundary.

Wayland role creation is a platform stub here. PE device probes cover the real
DXGI/WSI handshake, hidden-window visibility, and presented pixels.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--wine-src', type=Path, default=ROOT / 'thirdparty/wine-valve')
args = parser.parse_args()

def function(source, signature):
    start = source.index(signature)
    return source[start:source.index('\n}\n', start) + 3]

stubs = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
typedef uintptr_t HWND;
typedef int BOOL;
#define TRUE 1
#define FALSE 0
#define GA_ROOT 2
enum {WAYLAND_SURFACE_ROLE_NONE, WAYLAND_SURFACE_ROLE_TOPLEVEL,
      WAYLAND_SURFACE_ROLE_SUBSURFACE};
struct wayland_surface {int role;};
struct wayland_win_data {
    HWND hwnd;
    struct wayland_surface *wayland_surface;
    unsigned winehua_direct_early_refs;
    uint64_t winehua_direct_early_generation;
    BOOL winehua_direct_early_wsi;
};
static struct wayland_surface surface;
static struct wayland_win_data data;
static HWND root;
static int visible, missing, fail_create, locked, creations, cleanups;
static uint64_t winehua_direct_lease_generation;
static HWND NtUserGetAncestor(HWND hwnd, int mode) {
    assert(mode == GA_ROOT); return root;
}
static struct wayland_win_data *wayland_win_data_get(HWND hwnd) {
    assert(!locked);
    if (missing || hwnd != root) return NULL;
    locked = 1; return &data;
}
static void wayland_win_data_release(struct wayland_win_data *d) {
    assert(locked && d == &data); locked = 0;
}
static BOOL NtUserIsWindowVisible(HWND hwnd) {assert(hwnd == root);return visible;}
static BOOL wayland_surface_is_toplevel(struct wayland_surface *s) {
    return s->role == WAYLAND_SURFACE_ROLE_TOPLEVEL;
}
static BOOL wayland_win_data_create_wayland_surface(struct wayland_win_data *d, void *unused) {
    assert(locked && d == &data && !unused); creations++;
    if (fail_create) return FALSE;
    d->wayland_surface=&surface;
    if (!visible && !d->winehua_direct_early_wsi) {
        surface.role=WAYLAND_SURFACE_ROLE_NONE; cleanups++;
    } else surface.role=WAYLAND_SURFACE_ROLE_TOPLEVEL;
    return TRUE;
}
void winehua_release_direct_surface(HWND owner, uint64_t generation);
'''
main = r'''
static void reset(void) {
    data=(struct wayland_win_data){0}; surface=(struct wayland_surface){0};
    root=1;data.hwnd=root;visible=missing=fail_create=locked=creations=cleanups=0;
}
int main(void) {
    HWND owner1,owner2;
    uint64_t generation1,generation2;
    reset();
    assert(winehua_prepare_direct_surface(2,&owner1,&generation1));
    assert(owner1==root && generation1 && !visible && data.winehua_direct_early_wsi);
    assert(data.winehua_direct_early_refs==1 && creations==1 && !locked);
    assert(winehua_prepare_direct_surface(3,&owner2,&generation2));
    assert(owner2==root && generation2==generation1 && creations==1);
    winehua_release_direct_surface(owner1,generation1);
    assert(data.winehua_direct_early_refs==1 && !cleanups);
    winehua_release_direct_surface(owner2,generation2);
    assert(!data.winehua_direct_early_refs && !data.winehua_direct_early_wsi && cleanups==1);
    reset(); fail_create=1;
    assert(!winehua_prepare_direct_surface(1,&owner1,&generation1));
    assert(!owner1 && !generation1 && !data.winehua_direct_early_refs && !data.winehua_direct_early_wsi);
    reset();
    assert(winehua_prepare_direct_surface(1,&owner1,&generation1));
    /* Visible transition is set by real WindowPosChanged before normal hide. */
    visible=1;data.winehua_direct_early_wsi=FALSE;
    winehua_release_direct_surface(owner1,generation1);
    assert(!data.winehua_direct_early_refs && surface.role==WAYLAND_SURFACE_ROLE_TOPLEVEL && !cleanups);
    reset();visible=1;surface.role=WAYLAND_SURFACE_ROLE_TOPLEVEL;data.wayland_surface=&surface;
    assert(winehua_prepare_direct_surface(1,&owner1,&generation1));
    assert(!creations && !data.winehua_direct_early_wsi);
    winehua_release_direct_surface(owner1,generation1);assert(!cleanups);
    reset();visible=1;surface.role=WAYLAND_SURFACE_ROLE_SUBSURFACE;data.wayland_surface=&surface;
    assert(!winehua_prepare_direct_surface(1,&owner1,&generation1));
    assert(!owner1 && !generation1 && !creations && !data.winehua_direct_early_refs);
    reset();missing=1;
    assert(!winehua_prepare_direct_surface(1,&owner1,&generation1) && !locked);
    reset();assert(winehua_prepare_direct_surface(1,&owner1,&generation1));
    reset();assert(winehua_prepare_direct_surface(1,&owner2,&generation2));
    assert(generation1!=generation2);
    winehua_release_direct_surface(owner1,generation1);
    assert(data.winehua_direct_early_refs==1 && !cleanups);
    winehua_release_direct_surface(owner2,generation2);assert(cleanups==1);
    reset();assert(winehua_prepare_direct_surface(1,&owner1,&generation1));
    missing=1;winehua_release_direct_surface(owner1,generation1);assert(!locked);
    puts("PASS hidden/multiple/failed/visible/subsurface/missing/reused/destroyed Direct owner leases");
}
'''
source=(args.wine_src/'dlls/winewayland.drv/window.c').read_text()
code=stubs+function(source,'BOOL winehua_prepare_direct_surface(')+function(source,'void winehua_release_direct_surface(')+main
with tempfile.TemporaryDirectory(prefix='wine-direct-early-wsi-') as tmp:
    folder=Path(tmp);c=folder/'test.c';binary=folder/'test';c.write_text(code)
    subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-parameter',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',str(c),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,env={'ASAN_OPTIONS':'detect_leaks=0:halt_on_error=1'})
