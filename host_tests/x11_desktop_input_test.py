#!/usr/bin/env python3
"""Exercise actual NAPI hit-test/raise dispatch, including non-1 X11 ids."""
from pathlib import Path
import subprocess
ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'build/host_tests/x11-desktop-input'
BUILD.mkdir(parents=True, exist_ok=True)
source = (ROOT/'entry/src/main/cpp/bridge/napi_init.cpp').read_text()
def function(name):
    start = source.index('static napi_value '+name+'(')
    opening = source.index('{', start)
    depth, end = 1, opening+1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]
prelude = r"""
#include <cassert>
#include <cstdint>
#include <cstddef>
#include <iostream>
using napi_env=int; using napi_callback_info=int; using napi_value=void*;
using wl_fixed_t=int;
static uint32_t desktop=0, focused=0,raised=0,controlled=0;
static bool focusX11=false;
static int transforms=0;
static size_t inputCount=2;
static uint32_t presentation=0;
static bool isolated=false, surface=true;
void napi_get_cb_info(int,int,size_t* count,napi_value* args,void*,void*) {
 *count=*count<inputCount?*count:inputCount;
 args[0]=reinterpret_cast<void*>(40);if(*count>1)args[1]=reinterpret_cast<void*>(60);
 if(*count>2)args[2]=reinterpret_cast<void*>(uintptr_t(presentation));
}
void napi_get_value_int32(int,void* v,int32_t* out){*out=int32_t(uintptr_t(v));}
void napi_get_value_uint32(int,void* v,uint32_t* out){*out=uint32_t(uintptr_t(v));}
void napi_create_uint32(int,uint32_t v,void** out){*out=reinterpret_cast<void*>(uintptr_t(v));}
uint32_t winehua_x11_desktop_id(){return desktop;}
bool winehua_x11_is_desktop(uint32_t id){return desktop&&desktop==id;}
bool x11_toplevel_owns(uint32_t id){return id==40&&!desktop;}
void x11_toplevel_control(uint32_t id,int op,bool active){assert(op==0&&active);controlled=id;}
void winehua_display_focus_select(uint32_t id,bool x11){focused=id;focusX11=x11;}
class InputManager {public:
 static InputManager* GetInstance(){static InputManager m;return &m;}
 void CoordTransform(int x,int y,uint32_t root,int* wx,int* wy){
  assert(!desktop&&root==91);++transforms;*wx=x*2;*wy=y*2;
 }
};
int wl_fixed_to_int(int v){return v;}
class DesktopCompositor {public:
 bool IsToplevelIsolated(uint32_t id){assert(id==presentation);return isolated;}
};
class ToplevelManager {public:
 void* GetSurfaceForToplevel(uint32_t id){assert(id==presentation);return surface?this:nullptr;}
};
class WaylandServer {public:
 DesktopCompositor& GetDesktopCompositor(){static DesktopCompositor c;return c;}
 ToplevelManager& GetToplevelManager(){static ToplevelManager t;return t;}
 static WaylandServer* GetInstance(){static WaylandServer s;return &s;}
 uint32_t GetDesktopRootToplevelId(){assert(!desktop);return 91;}
 uint32_t FindToplevelAt(int x,int y){assert(x==80&&y==120&&!desktop);return 92;}
 void RaiseToplevel(uint32_t id,bool user){assert(user);raised=id;}
};
"""
main = r"""
int main(){
 // Wayland uses its renderer viewport and own child hit-test.
 assert(uintptr_t(FindToplevelAt(0,0))==92&&transforms==1);
 // X11 sessions may have any monotonically allocated desktop id. Neither
 // a fresh canvas nor a reopened canvas can enter the Wayland fallback.
 for(auto id:{40u,111u,900u}){
  desktop=id;assert(uintptr_t(FindToplevelAt(0,0))==id&&transforms==1);
 }
 // Isolated game input must retain its presentation owner even while a
 // separate X11 desktop exists. A removed surface must not receive input.
 inputCount=3;presentation=73;isolated=true;
 assert(uintptr_t(FindToplevelAt(0,0))==73&&transforms==1);
 surface=false;assert(uintptr_t(FindToplevelAt(0,0))==0&&transforms==1);
 isolated=false;surface=true;
 assert(uintptr_t(FindToplevelAt(0,0))==desktop&&transforms==1);
 inputCount=2;presentation=0;
 desktop=40;RaiseToplevel(0,0);assert(focused==40&&focusX11&&raised==0&&controlled==0);
 desktop=0;RaiseToplevel(0,0);assert(controlled==40&&raised==0);
 desktop=91;RaiseToplevel(0,0);assert(raised==40);
 desktop=0;assert(uintptr_t(FindToplevelAt(0,0))==92&&transforms==2);
 std::cout<<"PASS production desktop hit-test/raise: X11 ids, canvas reopen, multiwindow and Wayland fallback\n";
}
"""
cpp=BUILD/'probe.cpp';cpp.write_text(prelude+function('FindToplevelAt')+'\n'+function('RaiseToplevel')+main)
subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',str(cpp),'-o',str(BUILD/'probe')],check=True)
subprocess.run([str(BUILD/'probe')],check=True,timeout=10)
