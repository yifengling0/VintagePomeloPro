'''Dispatch production XDG menu requests through real libwayland + libffi.'''
from pathlib import Path
import argparse, subprocess, tempfile

R = Path(__file__).resolve().parents[1]
args = argparse.ArgumentParser()
args.add_argument('--baseline', action='store_true')
args = args.parse_args()
source = (R/'entry/src/main/cpp/compositor/xdg_shell.cpp').read_text()
begin = source.index('// Every advertised positioner')
end = source.index('static void xs_set_window_geometry', begin)
callbacks = source[begin:end]

def function(text, name):
    start = text.index('static void '+name+'(')
    brace = text.index('{', start); end = brace+1; depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}'); end += 1
    return text[start:end]

create = function(source, 'wm_create_positioner')
if args.baseline:
    old = subprocess.check_output(['git','show','HEAD:entry/src/main/cpp/compositor/xdg_shell.cpp'],cwd=R,text=True)
    create = function(old, 'wm_create_positioner')

header = r'''
#include "compositor/xdg_shell.h"
#include "compositor/xdg_positioner.h"
#include "protocols/xdg-shell-server-protocol.h"
#include <wayland-server-protocol.h>
#include <cassert>
#include <cstdio>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <array>
#include <sys/socket.h>
#include <unistd.h>
struct SurfaceData {
    wl_resource* parentSurface=nullptr;
    ResourceWatch parentSurfaceWatch{parentSurface};
    bool hasToplevel=false, isSubsurface=false;
    int32_t subsurfaceX=0, subsurfaceY=0;
    struct { int role=0; struct {int x=0,y=0;} contentRect; } committed;
};
int RoleFor(bool top,bool sub){return top?1:sub?2:0;}
struct ToplevelManager {
    std::mutex mutex;
    std::unordered_map<wl_resource*,SurfaceData*> surfaces;
    std::unique_lock<std::mutex> Lock(){return std::unique_lock<std::mutex>(mutex);}
    SurfaceData* FindRegisteredSurfaceDataLocked(wl_resource* s){
        auto it=surfaces.find(s);return it==surfaces.end()?nullptr:it->second;
    }
};
ToplevelManager manager;
ToplevelManager* gTmgr=&manager;
struct WaylandServer {
    wl_display* display=nullptr;
    int removed=0;
    static WaylandServer* GetInstance(){static WaylandServer ws;return &ws;}
    wl_display* GetDisplay(){return display;}
    void RemoveSubsurfaceRole(wl_resource* s){
        auto* sd=manager.FindRegisteredSurfaceDataLocked(s);assert(sd);
        sd->isSubsurface=false;sd->parentSurfaceWatch.Set(nullptr);++removed;
    }
};
'''
main = r'''
void message(int fd,uint32_t id,uint16_t opcode,std::initializer_list<uint32_t> args) {
    std::vector<uint32_t> data{id, uint32_t((args.size()+2)*4)<<16|opcode};
    data.insert(data.end(),args);assert(write(fd,data.data(),data.size()*4)==ssize_t(data.size()*4));
    assert(wl_event_loop_dispatch(wl_display_get_event_loop(WaylandServer::GetInstance()->display),10)>=0);
}
void surface_destroyed(wl_resource* r){
    delete static_cast<SurfaceData*>(wl_resource_get_user_data(r));manager.surfaces.erase(r);
}
int main(){
    int cases=0;
    std::array<int,4> order={0,1,2,3};
    do {
        int sockets[2];assert(socketpair(AF_UNIX,SOCK_STREAM,0,sockets)==0);
        auto* display=wl_display_create();auto* client=wl_client_create(display,sockets[0]);
        auto* ws=WaylandServer::GetInstance();ws->display=display;ws->removed=0;
        auto* wm=wl_resource_create(client,&xdg_wm_base_interface,3,2);
        wm_create_positioner(client,wm,3);
        auto* p=wl_client_get_object(client,3);
        message(sockets[1],3,1,{240,120}); // The old null dispatch table crashes here.
        message(sockets[1],3,2,{20,30,100,40});
        message(sockets[1],3,3,{6}); // bottom left anchor
        message(sockets[1],3,4,{8}); // bottom right gravity
        message(sockets[1],3,5,{3});
        message(sockets[1],3,6,{2,3});
        message(sockets[1],3,7,{});
        message(sockets[1],3,8,{800,600});
        message(sockets[1],3,9,{17});
        assert(positioner(p)->Complete());
        assert(positioner(p)->X()==22&&positioner(p)->Y()==73);
        assert(positioner(p)->reactive&&positioner(p)->parentConfigure==17);
        auto* parent=wl_resource_create(client,&wl_surface_interface,1,4);
        auto* child=wl_resource_create(client,&wl_surface_interface,1,5);
        for(auto* r:{parent,child}) {
            auto* sd=new SurfaceData;manager.surfaces[r]=sd;
            wl_resource_set_implementation(r,nullptr,sd,surface_destroyed);
        }
        auto* parentSd=manager.surfaces[parent];parentSd->hasToplevel=true;
        parentSd->committed.contentRect={4,7};
        auto* parentXdg=wl_resource_create(client,&xdg_surface_interface,3,6);
        auto* childXdg=wl_resource_create(client,&xdg_surface_interface,3,7);
        for(auto pair:{std::make_pair(parentXdg,parent),std::make_pair(childXdg,child)}){
            auto* data=new XdgSurface;data->wlSurfaceWatch.Set(pair.second);data->xdgSurface=pair.first;
            wl_resource_set_implementation(pair.first,nullptr,data,[](wl_resource* r){delete static_cast<XdgSurface*>(wl_resource_get_user_data(r));});
        }
        xs_get_popup(client,childXdg,8,parentXdg,p);
        auto* popup=wl_client_get_object(client,8);assert(popup);
        auto* childSd=manager.surfaces[child];
        assert(childSd->isSubsurface&&childSd->parentSurface==parent);
        assert(childSd->subsurfaceX==26&&childSd->subsurfaceY==80);
        // Reposition is real protocol dispatch; the popup snapshots the rules.
        message(sockets[1],3,6,{12,13});
        message(sockets[1],8,2,{3,99});
        assert(childSd->subsurfaceX==36&&childSd->subsurfaceY==90);
        message(sockets[1],3,0,{}); // destroy positioner, popup remains valid
        assert(static_cast<XdgPopupData*>(wl_resource_get_user_data(popup))->placement.offsetX==12);
        wl_resource* resources[]={parent,child,childXdg,popup};
        for(int i:order){
            wl_resource_destroy(resources[i]);
            if(i==0&&manager.surfaces.count(child))assert(!childSd->parentSurface);
        }
        wl_client_destroy(client);wl_display_destroy(display);close(sockets[1]);
        assert(manager.surfaces.empty());++cases;
    }while(std::next_permutation(order.begin(),order.end()));
    // All anchors/gravity combinations and extreme coordinates stay bounded.
    XdgPositionerState p;p.width=240;p.height=120;p.rectWidth=100;p.rectHeight=40;
    for(uint32_t a=0;a<=8;++a)for(uint32_t g=0;g<=8;++g){p.anchor=a;p.gravity=g;(void)p.X();(void)p.Y();}
    p.x=INT32_MAX;p.offsetX=INT32_MAX;assert(p.X()==INT32_MAX);
    p.x=INT32_MIN;p.offsetX=INT32_MIN;assert(p.X()==INT32_MIN);
    printf("PASS real Wayland/libffi positioner dispatch, popup placement/reposition, %d destruction orders\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='vp-xdg-popup-') as directory:
    temp=Path(directory)
    (temp/'test.cpp').write_text(header+callbacks+create+main)
    protocol=R/'entry/src/main/cpp/protocols/xdg-shell-protocol.c'
    subprocess.run(['gcc','-I',str(R/'entry/src/main/cpp'),'-c',str(protocol),'-o',str(temp/'protocol.o')],check=True)
    flags=subprocess.check_output(['pkg-config','--cflags','--libs','wayland-server'],text=True).split()
    command=['g++','-std=c++17','-Wall','-Wextra','-Wno-unused-function','-no-pie','-I',str(R/'entry/src/main/cpp'),str(temp/'test.cpp'),str(temp/'protocol.o'),*flags,'-o',str(temp/'test')]
    if not args.baseline:command[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
    subprocess.run(command,check=True)
    result=subprocess.run([str(temp/'test')],capture_output=True,text=True,timeout=30)
    if args.baseline:
        assert result.returncode==-11,result.stdout+result.stderr
        print('BASELINE RED: first xdg_positioner.set_size request crashes real libwayland dispatch')
    else:
        assert result.returncode==0,result.stdout+result.stderr
        print(result.stdout.strip())
