#!/usr/bin/env python3
"""Exercise cached X11 frame insets and actual GPU destination calculation."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
def function(path, signature):
    text = (root / path).read_text()
    start = text.index(signature)
    return text[start:text.index('\n}\n', start) + 2]

source = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
struct wlr_box {int x,y,width,height;};
struct wlr_xwayland_surface {unsigned width=968,height=667;uint32_t winehua_frame_extents[4]{};
 struct {int set_geometry;} events;};
struct x11_xs_entry {wlr_xwayland_surface* xs;int guestX=0,guestY=0,guestW=0,guestH=0;};
constexpr unsigned XCB_ATOM_CARDINAL=6;
struct xcb_get_property_reply_t {unsigned type=6,format=32,bytes_after=0;int size=16;uint32_t values[4]{4,4,23,4};};
int xcb_get_property_value_length(xcb_get_property_reply_t* r){return r->size;}
uint32_t* xcb_get_property_value(xcb_get_property_reply_t* r){return r->values;}
void wl_signal_emit_mutable(int*,void*){}
PARSER
DESTINATION
int main(){
 wlr_xwayland_surface xs;x11_xs_entry e{&xs};wlr_box b;
 xcb_get_property_reply_t reply;read_surface_frame_extents(&xs,&reply);
 assert(GuestDestination(&e,1936,1334,&b));
 assert(b.x==8&&b.y==46&&b.width==1920&&b.height==1280);
 e.guestX=10;e.guestY=20;e.guestW=100;e.guestH=80;
 assert(GuestDestination(&e,968,667,&b)&&b.x==14&&b.y==43&&b.width==100&&b.height==80);
 e.guestX=969;assert(!GuestDestination(&e,968,667,&b));
 e.guestX=0;e.guestY=0;e.guestW=0;e.guestH=0;
 xs.width=0;assert(!GuestDestination(&e,100,100,&b));xs.width=968;
 reply.values[0]=1000;read_surface_frame_extents(&xs,&reply);
 assert(!GuestDestination(&e,968,667,&b));
 // Invalid/deleted properties clear old offsets; no stale title strip in fullscreen.
 for(int mode=0;mode<4;++mode){
  reply={};if(mode==0)reply.format=8;if(mode==1)reply.size=12;
  if(mode==2)reply.bytes_after=4;
  if(mode==3)reply.values[0]=0xffffffff;
  read_surface_frame_extents(&xs,&reply);
  assert(GuestDestination(&e,968,667,&b)&&b.x==0&&b.y==0&&b.width==968&&b.height==667);
 }
 reply={};reply.values[0]=reply.values[1]=reply.values[2]=reply.values[3]=0;
 read_surface_frame_extents(&xs,&reply);
 assert(GuestDestination(&e,968,667,&b)&&b.x==0&&b.y==0&&b.width==968&&b.height==667);
}
'''.replace('PARSER', function('thirdparty/wlroots/xwayland/xwm.c', 'static void read_surface_frame_extents(')).replace('DESTINATION', function('entry/src/main/cpp/display/x11_toplevel.c', 'static bool GuestDestination('))

with tempfile.TemporaryDirectory(prefix='x11-multi-client-') as tmp:
    p = Path(tmp)
    (p / 'test.cpp').write_text(source)
    subprocess.run(['g++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    str(p / 'test.cpp'), '-o', str(p / 'test')], check=True)
    subprocess.run([str(p / 'test')], check=True, timeout=10)
print('PASS X11 production property cache/GPU destination: decorations, scale, subregion, borderless, malformed/deleted extents and zero size')
