#!/usr/bin/env python3
"""Exercise the real X11 CSD request handlers, geometry and lifecycle guards."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'entry/src/main/cpp/display/x11_toplevel.c').read_text()
def function(signature):
    start = source.index(signature)
    if ';' in source[start:source.index('{', start)]:
        start = source.index(signature, start + len(signature))
    return source[start:source.index('\n}\n', start) + 2]

code = r'''
#include <cassert>
#include <cstdint>
#include <climits>
#include <pthread.h>
extern "C" {
#include <wayland-server-core.h>
}
struct xcb_size_hints_t {uint32_t flags=0;int min_width=0,min_height=0,max_width=0,max_height=0;};
enum {XCB_ICCCM_SIZE_HINT_P_MIN_SIZE=16,XCB_ICCCM_SIZE_HINT_P_MAX_SIZE=32};
enum {WLR_EDGE_TOP=1,WLR_EDGE_BOTTOM=2,WLR_EDGE_LEFT=4,WLR_EDGE_RIGHT=8};
struct wlr_box {int x,y,width,height;};
struct wlr_xwayland_surface {
 void* surface=(void*)1; bool minimized=false,fullscreen=false,maximized_horz=false,maximized_vert=false;
 int x=100,y=200,width=400,height=300; xcb_size_hints_t* size_hints=nullptr;
};
struct x11_xs_entry {
 bool dead=false,createdPosted=true,is_popup=false;void* win=(void*)1;wlr_xwayland_surface* xs;
 bool pointer_left_down=false,move_pending=false;uint32_t resize_edges=0,toplevelId=5;
 int pointer_x=10,pointer_y=20,resize_pointer_x=0,resize_pointer_y=0;
 wlr_box resize_start{},resize_last{};wl_listener request_move{},request_resize{},request_maximize{};
};
struct wlr_xwayland_resize_event {wlr_xwayland_surface* surface;uint32_t edges;};
static pthread_mutex_t g_lock=PTHREAD_MUTEX_INITIALIZER;
static unsigned moves=0,ends=0,maxes=0;static bool maximized=false;
void x11_toplevel_bridge_post_move(uint32_t id,bool active){assert(id==5);active?++moves:++ends;}
void x11_toplevel_bridge_post_maximized(uint32_t id,bool state){assert(id==5);++maxes;maximized=state;}
void wlr_xwayland_surface_set_maximized(wlr_xwayland_surface*,bool a,bool b){assert(a==b);}
FUNCTIONS
int main(){
 wlr_xwayland_surface xs;x11_xs_entry e{};e.xs=&xs;
 handle_request_move(&e.request_move,nullptr);assert(!moves); // late/keyboard request without a press
 e.pointer_left_down=true;handle_request_move(&e.request_move,nullptr);assert(moves==1&&e.move_pending);
 handle_request_move(&e.request_move,nullptr);assert(moves==1); // no duplicate native start
 EndInteraction(&e);assert(ends==1&&!e.pointer_left_down&&!e.move_pending);
 handle_request_move(&e.request_move,nullptr);assert(moves==1);
 for(int reason=0;reason<6;++reason){
  e.pointer_left_down=true;
  e.dead=reason==0;e.createdPosted=reason!=1;e.is_popup=reason==2;
  e.win=reason==3?nullptr:(void*)1;xs.minimized=reason==4;xs.fullscreen=reason==5;
  handle_request_move(&e.request_move,nullptr);assert(moves==1);
 }
 e.dead=false;e.createdPosted=true;e.is_popup=false;e.win=(void*)1;xs.minimized=xs.fullscreen=false;
 assert(!ValidResizeEdges(0)&&!ValidResizeEdges(16)&&!ValidResizeEdges(3)&&!ValidResizeEdges(12));
 for(unsigned edges:{1,2,4,8,5,6,9,10}){
  EndInteraction(&e);e.pointer_left_down=true;
  wlr_xwayland_resize_event ev{&xs,edges};handle_request_resize(&e.request_resize,&ev);
  assert(e.resize_edges==edges&&e.resize_pointer_x==110&&e.resize_pointer_y==220);
  auto b=ResizeBounds(&e,150,250);
  assert(b.width==400+((edges&8)?40:0)-((edges&4)?40:0));
  assert(b.height==300+((edges&2)?30:0)-((edges&1)?30:0));
  if(edges&4)assert(b.x+b.width==500);
  if(edges&1)assert(b.y+b.height==500);
  b=ResizeBounds(&e,INT_MAX,INT_MIN);assert(b.width>0&&b.width<=32767&&b.height>0&&b.height<=32767);
 }
 xcb_size_hints_t hints{};hints.flags=XCB_ICCCM_SIZE_HINT_P_MIN_SIZE|XCB_ICCCM_SIZE_HINT_P_MAX_SIZE;
 hints.min_width=200;hints.min_height=150;hints.max_width=800;hints.max_height=600;xs.size_hints=&hints;
 e.resize_edges=WLR_EDGE_LEFT|WLR_EDGE_TOP;
 auto b=ResizeBounds(&e,2000,2000);assert(b.width==200&&b.height==150&&b.x==300&&b.y==350);
 b=ResizeBounds(&e,-2000,-2000);assert(b.width==800&&b.height==600&&b.x==-300&&b.y==-100);
 EndInteraction(&e);wlr_xwayland_resize_event ev{&xs,8};handle_request_resize(&e.request_resize,&ev);assert(!e.resize_edges);
 xs.maximized_horz=xs.maximized_vert=true;handle_request_maximize(&e.request_maximize,nullptr);assert(maximized&&maxes==1);
 xs.maximized_horz=xs.maximized_vert=false;handle_request_maximize(&e.request_maximize,nullptr);assert(!maximized&&maxes==2);
}
'''
names=['CanInteract','EndInteraction','handle_request_move','ValidResizeEdges','handle_request_resize','ResizeBounds','handle_request_maximize']
signatures=['static bool ','static void ','static void ','static bool ','static void ','static struct wlr_box ','static void ']
code=code.replace('FUNCTIONS','\n'.join(function(a+b+'(') for a,b in zip(signatures,names)))
code=code.replace('const struct wlr_xwayland_resize_event *ev = data;',
                  'const struct wlr_xwayland_resize_event *ev = (const struct wlr_xwayland_resize_event *)data;')
code=code.replace('#include <climits>','#include <climits>\n#include <initializer_list>')
with tempfile.TemporaryDirectory(prefix='x11-controls-') as tmp:
    p=Path(tmp);(p/'test.cpp').write_text(code)
    subprocess.run(['g++','-std=c++20','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer',str(p/'test.cpp'),'-pthread','-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True,timeout=10)
print('PASS production X11 CSD controls: late/duplicate requests, dead/popup/minimized/fullscreen guards, all eight resize directions, hints, overflow and maximize/restore')
