'''Replay actual XDG destroy callbacks with real libwayland in every order.'''
from pathlib import Path
import subprocess, tempfile, itertools
R = Path(__file__).resolve().parents[1]
source = (R/'entry/src/main/cpp/compositor/xdg_shell.cpp').read_text()
def function(name):
    start=source.index('static void '+name+'(')
    brace=source.index('{',start); end=brace+1; depth=1
    while depth:
        depth += (source[end]=='{')-(source[end]=='}'); end+=1
    return source[start:end]
header = r'''
#include "compositor/xdg_shell.h"
#include <wayland-server-protocol.h>
#include <cassert>
#include <algorithm>
#include <array>
#include <cstdio>
#include <sys/socket.h>
#include <unistd.h>
#define OH_LOG_INFO(...) ((void)0)
struct SurfaceData {
    bool hasToplevel=true, isSubsurface=false;
    uint32_t toplevelId=17;
    struct {int role=1; bool hasWindowGeometry=true;} committed;
};
int RoleFor(bool top, bool sub) {return top?1:(sub?2:0);}
enum class ToplevelEventType {Destroyed};
struct WaylandServer {
    int cleaned=0, notified=0, unregistered=0;
    static WaylandServer* GetInstance(){static WaylandServer ws;return &ws;}
    void OnToplevelDestroyed(uint32_t id){assert(id==17);++cleaned;}
    void PostToplevelEvent(uint32_t id,ToplevelEventType){assert(id==17);++notified;}
    void UnregisterToplevelResource(uint32_t id){assert(id==17);++unregistered;}
};
'''
main = r'''
int main() {
  int cases=0;
  for(int disconnect=0;disconnect<2;++disconnect) {
    std::array<int,3> creation={0,1,2};
    do {
      std::array<int,3> destruction={0,1,2};
      do {
        auto* ws=WaylandServer::GetInstance();*ws={};
        int sockets[2];assert(socketpair(AF_UNIX,SOCK_STREAM,0,sockets)==0);
        auto* display=wl_display_create();auto* client=wl_client_create(display,sockets[0]);
        wl_resource* resources[3]{};
        for(auto i:creation) resources[i]=wl_resource_create(client,&wl_surface_interface,1,0);
        auto* sd=new SurfaceData;
        auto* xdg=new XdgSurface;
        xdg->wlSurfaceWatch.Set(resources[0]);xdg->xdgSurface=resources[1];
        xdg->xdgToplevelWatch.Set(resources[2]);xdg->toplevelId=17;
        auto* td=new ToplevelData;td->toplevelId=17;td->xdgSurfaceWatch.Set(resources[1]);
        wl_resource_set_implementation(resources[0],nullptr,sd,[](wl_resource* r){
          auto* sd=static_cast<SurfaceData*>(wl_resource_get_user_data(r));
          if(sd->hasToplevel){auto* ws=WaylandServer::GetInstance();
            ws->OnToplevelDestroyed(sd->toplevelId);
            ws->PostToplevelEvent(sd->toplevelId,ToplevelEventType::Destroyed);}
          delete sd;
        });
        wl_resource_set_implementation(resources[1],nullptr,xdg,xs_resource_destroy);
        wl_resource_set_implementation(resources[2],nullptr,td,tl_resource_destroy);
        if(!disconnect) for(int i:destruction) {
          if(i==1) xs_destroy(client,resources[i]);else wl_resource_destroy(resources[i]);
          // The other role must immediately lose its reference, before reuse.
          if(i==0 && resources[1]) assert(!xdg->wlSurface);
          if(i==1 && resources[2]) assert(!td->xdgSurface);
          if(i==2 && resources[1]) assert(!xdg->xdgToplevel);
          resources[i]=nullptr;
        }
        wl_client_destroy(client);
        assert(ws->cleaned==1 && ws->notified==1 && ws->unregistered==1);
        wl_display_destroy(display);close(sockets[1]);++cases;
      }while(std::next_permutation(destruction.begin(),destruction.end()));
    }while(std::next_permutation(creation.begin(),creation.end()));
  }
  std::printf("PASS %d real libwayland XDG destruction/disconnect cases, one cleanup/event each\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='vp-xdg-lifetime-') as temp:
    p=Path(temp);(p/'test.cpp').write_text(header+'\n'+function('tl_resource_destroy')+'\n'+function('xs_resource_destroy')+'\n'+function('xs_destroy')+'\n'+main)
    flags=subprocess.check_output(['pkg-config','--cflags','--libs','wayland-server'],text=True).split()
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie','-I',str(R/'entry/src/main/cpp'),str(p/'test.cpp'),*flags,'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True,timeout=30)
