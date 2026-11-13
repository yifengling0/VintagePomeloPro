"""Execute production placement code with USER coordinate stubs.

The real Win32 API/device probe in scripts/probes/layered_child_probe.c covers
pixels and full driver lifecycle. Here only the production placement prefixes
are compiled, before the GDI drawing and Wayland protocol side effects.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--wine-src', type=Path, default=ROOT / 'thirdparty/wine-valve')
parser.add_argument('--expect-baseline-failure', action='store_true')
parser.add_argument('--baseline-root-only', action='store_true')
args = parser.parse_args()

stubs = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef int BOOL, HWND, HDC, HRGN, UINT;
typedef unsigned DWORD, COLORREF;
typedef unsigned char BYTE;
typedef struct {int x,y;} POINT;
typedef struct {int cx,cy;} SIZE;
typedef struct {int left,top,right,bottom;} RECT;
typedef struct {BYTE BlendOp,BlendFlags,SourceConstantAlpha,AlphaFormat;} BLENDFUNCTION;
struct window_rects {RECT window,client,visible;};
struct window_surface {int unused;};
struct wayland_surface {int unused;};
struct wayland_client_surface {int unused;};
struct wayland_win_data {struct window_rects rects; struct wayland_surface *wayland_surface; BOOL is_fullscreen,managed;};
#define WINAPI
#define TRUE 1
#define FALSE 0
#define TRACE(...) ((void)0)
#define SWP_NOSIZE 1
#define SWP_NOMOVE 2
#define SWP_NOZORDER 4
#define SWP_NOACTIVATE 8
#define SWP_NOREDRAW 16
#define WINE_SWP_FULLSCREEN 32
#define ULW_COLORKEY 1
#define ULW_ALPHA 2
#define ULW_OPAQUE 4
#define ULW_EX_NORESIZE 8
#define GWL_EXSTYLE 1
#define WS_EX_LAYERED 0x80000
#define GA_PARENT 1
#define GA_ROOT 2
#define COORDS_PARENT 1
#define MDT_RAW_DPI 2
#define ERROR_INVALID_PARAMETER 87
#define ERROR_INCORRECT_SIZE 1460
static struct window_rects current;
static struct window_surface pixels;
static struct wayland_surface parent_surface;
static struct wayland_win_data child_data,parent_data;
static int parent=1,root=1,origin_x=40,origin_y=80,map_calls,applied,last_root,managed;
static UINT get_thread_dpi(void) {return 144;}
static HWND NtUserGetAncestor(HWND h,UINT which) {return which==GA_PARENT?parent:root;}
static HWND NtUserGetDesktopWindow(void) {return 99;}
static int NtUserMapWindowPoints(HWND from,HWND to,POINT *p,UINT count,UINT dpi) {
    assert(dpi==144 && count>=1 && count<=2);
    assert((from==0&&to==parent)||(from==parent&&to==0));
    map_calls++;
    for(UINT i=0;i<count;i++) {p[i].x+=(from?1:-1)*origin_x;p[i].y+=(from?1:-1)*origin_y;}
    return 1;
}
static DWORD get_window_long(HWND h,UINT f) {return WS_EX_LAYERED;}
static BOOL NtUserGetLayeredWindowAttributes(HWND h,void *a,void *b,void *c) {return FALSE;}
static void RtlSetLastWin32Error(UINT e) {}
static void get_window_rects(HWND h,UINT relative,struct window_rects *r,UINT dpi) {*r=current;}
static void OffsetRect(RECT *r,int x,int y) {r->left+=x;r->right+=x;r->top+=y;r->bottom+=y;}
static struct window_surface *get_window_surface(HWND h,UINT f,BOOL l,struct window_rects *r,RECT *s) {return &pixels;}
static BOOL apply_window_pos(HWND h,HWND after,UINT f,struct window_surface *s,const struct window_rects *r,const RECT *v) {current=*r; applied++; return TRUE;}
static BOOL is_window_managed(HWND h,UINT f,BOOL fs) {return managed;}
static struct wayland_win_data *wayland_win_data_get(HWND h) {return &child_data;}
static struct wayland_win_data *wayland_win_data_get_nolock(HWND h) {assert(h==root);return &parent_data;}
static UINT NtUserGetWinMonitorDpi(HWND h,UINT type) {assert(type==MDT_RAW_DPI);return 144;}
'''

win_source = ROOT / 'thirdparty/wine-valve' if args.baseline_root_only else args.wine_src
win = (win_source / 'dlls/win32u/window.c').read_text()
start = win.index('BOOL WINAPI NtUserUpdateLayeredWindow(')
ulw = win[start:win.index('    if (!surface) return FALSE;', start)]
ulw += '    return surface != 0;\n}\n'
drv = (args.wine_src / 'dlls/winewayland.drv/window.c').read_text()
start = drv.index('void WAYLAND_WindowPosChanged(')
callback = drv[start:drv.index('    if (!surface)\n', start)]
callback += '    last_root = toplevel;\n}\n'

main = r'''
static void reset(int x,int y) {
    current.window=(RECT){x,y,x+96,y+72}; current.client=current.visible=current.window;
    map_calls=applied=0;
}
int main(void) {
    reset(30,40);
    SIZE size={96,72};
    for(int i=0;i<20;i++) {
        POINT at={current.window.left+origin_x,current.window.top+origin_y};
        assert(NtUserUpdateLayeredWindow(2,0,&at,&size,0,0,0,0,ULW_ALPHA,0));
        assert(current.window.left==30 && current.window.top==40);
    }
    assert(map_calls==20 && applied==20);
    origin_x=-120; origin_y=210; reset(12,18);
    POINT at={-108,228};
    assert(NtUserUpdateLayeredWindow(2,0,&at,0,0,0,0,0,ULW_ALPHA,0));
    assert(current.window.left==12 && current.window.top==18);
    int maps=map_calls;
    size=(SIZE){100,80};
    assert(NtUserUpdateLayeredWindow(2,0,0,&size,0,0,0,0,ULW_ALPHA,0));
    assert(current.window.left==12 && current.window.top==18 && map_calls==maps);
    size=(SIZE){0,80};
    assert(!NtUserUpdateLayeredWindow(2,0,0,&size,0,0,0,0,ULW_ALPHA,0));
    parent=99; reset(247,0); at=(POINT){247,0};
    assert(NtUserUpdateLayeredWindow(2,0,&at,0,0,0,0,0,ULW_ALPHA,0));
    assert(current.window.left==247 && current.window.top==0 && map_calls==0);
    parent=1; root=1; origin_x=40;origin_y=80;reset(30,40);
    parent_data.wayland_surface=&parent_surface;
    for(int hint=0;hint<4;hint++) {
        WAYLAND_WindowPosChanged(2,0,hint,0,&current,&pixels);
        assert(last_root==1 && child_data.rects.window.left==70 && child_data.rects.window.top==120);
        assert(current.window.left==30 && current.window.top==40);
    }
    root=2; managed=0; parent=99; reset(247,0);
    WAYLAND_WindowPosChanged(2,0,2,0,&current,&pixels);
    assert(last_root==2 && child_data.rects.window.left==247);
    root=2; managed=1;
    WAYLAND_WindowPosChanged(2,0,0,0,&current,&pixels);
    assert(last_root==2);
    puts("PASS repeated ULW, hidden/nested coordinates, size-only, top-level, real child root and RAW-DPI geometry");
}
'''
with tempfile.TemporaryDirectory(prefix='vp-layered-child-') as temp:
    temp=Path(temp)
    source=temp/'test.c'; binary=temp/'test'
    source.write_text(stubs+ulw+callback+main)
    subprocess.run(['cc','-std=c11','-O1','-Wall','-Wextra','-Wno-unused-parameter','-Wno-unused-variable',str(source),'-o',str(binary)],check=True)
    result=subprocess.run([str(binary)],capture_output=True,text=True)
    if args.expect_baseline_failure:
        expected = 'last_root==1' if args.baseline_root_only else 'current.window.left==30'
        assert result.returncode != 0 and expected in result.stderr,result.stdout+result.stderr
        print('BASELINE RED: production ' + ('Wayland child loses its root when owner_hint is absent' if args.baseline_root_only else 'UpdateLayeredWindow adds parent origin on the first refresh'))
    else:
        assert result.returncode == 0,result.stdout+result.stderr
        print(result.stdout.strip())
