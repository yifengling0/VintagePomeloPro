#!/usr/bin/env python3
"""Run the production bridge with deterministic consumer fakes, no OHOS device."""
import pathlib, subprocess, tempfile
root = pathlib.Path(__file__).resolve().parents[1]
(root / 'build').mkdir(exist_ok=True)
files = {
'native_window/external_window.h': '#pragma once\nstruct OHNativeWindow {};\nenum {SET_TIMEOUT};\ninline int OH_NativeWindow_NativeWindowHandleOpt(OHNativeWindow*,int,int){return 0;}\n',
'native_buffer/native_buffer.h': '#define NATIVEBUFFER_USAGE_HW_RENDER 1\n#define NATIVEBUFFER_USAGE_HW_TEXTURE 2\n',
'native_image/native_image.h': '''#pragma once
#include <native_window/external_window.h>
struct OH_NativeImage { OHNativeWindow win; };
inline int images=0;
inline OH_NativeImage* OH_ConsumerSurface_Create(){ ++images; return new OH_NativeImage; }
inline void OH_NativeImage_Destroy(OH_NativeImage** p){ delete *p; *p=nullptr; --images; }
inline void OH_ConsumerSurface_SetDefaultSize(OH_NativeImage*,int,int){}
inline void OH_ConsumerSurface_SetDefaultUsage(OH_NativeImage*,int){}
inline OHNativeWindow* OH_NativeImage_AcquireNativeWindow(OH_NativeImage* p){return &p->win;}
''',
'graphics/egl_renderer.h': '''#pragma once
#include <native_image/native_image.h>
struct DesktopCompositor {};
inline int renderers=0, retired=0;
class EglRenderer { bool live=false; public:
 EglRenderer(DesktopCompositor&){} ~EglRenderer(){Shutdown();}
 void Shutdown(){if(live){--renderers;++retired;live=false;}}
 void SetIsolatedToplevel(bool){} void SetToplevelId(unsigned){}
 bool Init(OHNativeWindow*,int,int){live=true;++renderers;return true;}
 void SetPaused(bool){}
};
''',
'compositor/wayland_server.h': '''#pragma once
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include "graphics/egl_renderer.h"
struct SurfaceData { bool hasToplevel=true; unsigned toplevelId=20, clientPid=100; };
inline SurfaceData source;
struct wl_resource {};
inline wl_resource resource;
inline unsigned long long sourceKey=1000;
enum class ToplevelEventType {Created,Destroyed,Resize,Restored,Minimized};
struct ToplevelEventBus {
 static std::string JsonCreatedAt(int,int,int,int,unsigned){return "";}
 static std::string JsonResize(int,int){return "";}
};
class ToplevelManager {unsigned next=50;std::mutex m; public:
 auto Lock(){return std::unique_lock<std::mutex>(m);}
 unsigned AllocateToplevelId(){return next++;}
 auto SurfaceResources(){return std::map<unsigned long long,wl_resource*>{{sourceKey,&resource}};}
 SurfaceData* FindRegisteredSurfaceDataLocked(wl_resource*){return &source;}
};
class WaylandServer {ToplevelManager tm;DesktopCompositor c;public:
 std::vector<std::pair<unsigned,ToplevelEventType>> events;
 static WaylandServer* GetInstance(){static WaylandServer s;return &s;}
 auto& GetToplevelManager(){return tm;} auto& GetDesktopCompositor(){return c;}
 void PostToplevelEvent(unsigned id,ToplevelEventType t,std::string={}){events.emplace_back(id,t);}
};
''',
'input/input_manager.h': '''#pragma once
struct Point {unsigned id=0;double x=0,y=0;int action=0;} ;
inline Point point;
class InputManager { public:
 static InputManager* GetInstance(){static InputManager i;return &i;}
 void SendPointerEvent(unsigned id,int a,double x,double y,unsigned,double,double,bool,bool local){ if(!local)__builtin_abort();point={id,x,y,a}; }
 void SendScrollEvent(unsigned id,int,double,int,double x,double y,bool local){if(!local)__builtin_abort();point={id,x,y,9};}
};
''',
'ohos_buffer.h': '''#pragma once
struct OH_NativeImage;struct wlr_buffer{};
extern "C" wlr_buffer* wl_ohos_consumer_buffer_acquire(OH_NativeImage*);
extern "C" void wl_ohos_consumer_buffer_drop(wlr_buffer*);
''',
'ohos_egl_import.h': '#pragma once\ninline bool wl_ohos_egl_active(){return true;}\n',
}
with tempfile.TemporaryDirectory(prefix='x11-host-test-', dir=root/'build') as tmp:
    temp=pathlib.Path(tmp)
    for path, content in files.items():
        p=temp/path;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(content)
    for name in ['x11_external_host.h','display_session.h','desktop_fit.h','ohos_output.h']:
        (temp/name).write_bytes((root/'entry/src/main/cpp/display'/name).read_bytes())
    # Exact production translation unit next to fakes, no algorithm copy.
    (temp/'bridge.cpp').write_bytes((root/'entry/src/main/cpp/display/x11_external_host.cpp').read_bytes())
    (temp/'test.cpp').write_text(r"""
#include "compositor/wayland_server.h"
#include "input/input_manager.h"
#include "x11_external_host.h"
#include "display_session.h"
#include "desktop_fit.h"
#include "ohos_buffer.h"
#include <cassert>
#include <iostream>
static winehua_x11_window host{0,32,200,1,100,100,400,300,true};
static bool present=true;static unsigned focus=0,frames=0,clears=0,acquired=0,dropped=0;
extern "C" size_t wl_ohos_output_snapshot_windows(winehua_x11_window* out,size_t){ if(!present)return 0;*out=host;return 1; }
extern "C" int wl_ohos_output_client_xwindow_generation(unsigned xid,uint64_t* gen){if(!present||xid!=host.xid)return 0;*gen=host.generation;return 1;}
extern "C" int wl_ohos_output_client_frame_set(unsigned xid,wlr_buffer*,int,uint64_t){assert(xid==host.xid);++frames;return 1;}
extern "C" void wl_ohos_output_client_frame_clear(unsigned){++clears;}
extern "C" int wl_ohos_output_client_frame_region(unsigned,int,int,int,int){return 1;}
extern "C" int wl_ohos_output_desktop_rect(int*x,int*y,int*w,int*h){*x=*y=0;*w=800;*h=600;return 1;}
extern "C" void winehua_x11_canvas_dimensions(int*w,int*h){*w=800;*h=600;}
extern "C" bool winehua_x11_is_desktop(unsigned id){return id==1;}
extern "C" bool x11_toplevel_input_position(unsigned,double,double,double*,double*){return false;}
extern "C" void winehua_display_focus_select(unsigned id,bool x11){assert(!x11);focus=id;}
extern "C" wlr_buffer* wl_ohos_consumer_buffer_acquire(OH_NativeImage*){static wlr_buffer f;++acquired;return &f;}
extern "C" void wl_ohos_consumer_buffer_drop(wlr_buffer*){++dropped;}
extern "C" int WineHua_ComputeDesktopFit(int,int,int,int,DesktopFitRect* f){f->scale=1;return 1;}
extern "C" double WineHua_FitUnmapX(const DesktopFitRect*,double x){return x;}
extern "C" double WineHua_FitUnmapY(const DesktopFitRect*,double y){return y;}
int main(){
 auto* s=WaylandServer::GetInstance();
 winehua_x11_hosts_tick();assert(s->events.size()==1);unsigned id=s->events.back().first;
 assert(!winehua_x11_bind_external(20,id,999,200,20,30,200,100));
 assert(!winehua_x11_bind_external(20,id,100,999,20,30,200,100));
 assert(winehua_x11_bind_external(20,id,100,200,20,30,200,100));assert(renderers==0);
 winehua_x11_hosts_tick();assert(renderers==1&&images==1&&frames==1&&acquired==dropped);
 assert(winehua_x11_external_pointer(1,1,150,150,272,0,0,true));
 assert(point.id==20&&point.x==30&&point.y==20&&focus==20);
 assert(winehua_x11_external_pointer(1,2,799,599,272,0,0,true));assert(point.action==2);
 assert(!winehua_x11_external_pointer(1,1,799,599,272,0,0,true));
 assert(winehua_x11_external_scroll(1,0,0.25,-1,150,150));assert(point.action==9);
 assert(winehua_x11_external_pointer(1,1,150,150,272,0,0,true));
 winehua_x11_unbind_external(id);assert(renderers==1);winehua_x11_hosts_tick();assert(renderers==0&&images==0);
 assert(point.action==2); // unbinding while pressed releases the live source
 assert(winehua_x11_bind_external(20,id,100,200,20,30,200,100));winehua_x11_hosts_tick();
 ++sourceKey;winehua_x11_hosts_tick();assert(renderers==0&&images==0);
 assert(winehua_x11_bind_external(20,id,100,200,20,30,200,100));winehua_x11_hosts_tick();
 unsigned before=clears;host.generation=2;winehua_x11_hosts_tick();assert(renderers==0&&clears==before);
 assert(!winehua_x11_host_owns(id));assert(!winehua_x11_bind_external(20,id,100,200,20,30,200,100));
 unsigned replacement=s->events[s->events.size()-2].first;
 assert(replacement!=id&&winehua_x11_host_owns(replacement));
 winehua_x11_hosts_shutdown();assert(renderers==0&&images==0&&acquired==dropped);
 std::cout<<"PASS production X11 host: PID identity, deferred teardown, source/XID reuse, captured release, fractional scroll, buffer balance\n";
}
""")
    subprocess.run(['c++','-std=c++17','-pthread','-I'+str(temp),str(temp/'bridge.cpp'),str(temp/'test.cpp'),'-o',str(temp/'test')],check=True)
    subprocess.run([str(temp/'test')],check=True)
