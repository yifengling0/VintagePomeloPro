#!/usr/bin/env python3
"""Run production scene listeners with real Wayland signals and queue retirement."""
from pathlib import Path
import shlex, subprocess
ROOT=Path(__file__).resolve().parents[1]
BUILD=ROOT/'build/host_tests/x11-surface-lifecycle'
BUILD.mkdir(parents=True,exist_ok=True)
def function(path,signature):
    source=(ROOT/path).read_text();a=source.index(signature);b=source.index('{',a)+1;depth=1
    while depth:
        depth+=(source[b]=='{')-(source[b]=='}');b+=1
    return source[a:b]+'\n'
prelude=r"""
#include <cassert>
#include <cstdint>
#include <iostream>
#include <pthread.h>
extern "C" {
#include <wayland-server-core.h>
}
#define OHLOG(...) ((void)0)
#define OH_LOG_ERROR(...) ((void)0)
struct wlr_scene_node {struct {wl_signal destroy;} events;bool enabled=true;};
struct wlr_scene_buffer {wlr_scene_node node;void* buffer=nullptr;};
struct wlr_scene_surface {wlr_scene_buffer* buffer;};
struct wlr_surface {struct {uint32_t seq=1;} current;};
struct wlr_xwayland_surface {wlr_surface* surface;};
struct Scene {int tree;};
struct {Scene* scene=nullptr;bool multiwindow=false;} g_out;
struct ohos_client_surface {
 wlr_xwayland_surface* xs=nullptr;wl_listener associate{},dissociate{},scene_node_destroy{};
 wlr_scene_surface* scene_surf=nullptr;uint32_t lastSeq=0;uint64_t generation=1;
 wlr_scene_buffer* frame_node=nullptr;
};
static bool failCreate=false;
wlr_scene_surface* wlr_scene_surface_create(int*,wlr_surface*){
 if(failCreate)return nullptr;
 auto* s=new wlr_scene_surface{new wlr_scene_buffer{}};
 wl_signal_init(&s->buffer->node.events.destroy);return s;
}
void DestroyFrameNode(ohos_client_surface*,const char*){}
void DestroyScene(wlr_scene_surface* s){
 wl_signal_emit_mutable(&s->buffer->node.events.destroy,nullptr);
 assert(wl_list_empty(&s->buffer->node.events.destroy.listener_list));
 delete s->buffer;delete s;
}
static ohos_client_surface* top=nullptr;
struct ohos_child_face {uint64_t generation;wlr_scene_buffer* frame_node;};
static ohos_child_face* child=nullptr;
static unsigned releases=0,retired=0;static bool queueAlive=true;
ohos_client_surface* FindClientByWindow(uint32_t){return top;}
ohos_child_face* ChildFaceFind(uint32_t){return child;}
void ChildFaceDestroy(ohos_child_face* f,const char*){
 assert(queueAlive);++retired;f->frame_node=nullptr;child=nullptr;
}
void wlr_scene_buffer_set_buffer(wlr_scene_buffer* b,void* v){assert(queueAlive);b->buffer=v;++releases;}
void wlr_scene_node_set_enabled(wlr_scene_node* n,bool e){n->enabled=e;}
static pthread_mutex_t g_lock=PTHREAD_MUTEX_INITIALIZER;
struct x11_xs_entry {uint64_t generation=5;void* guestTexture=nullptr;void* guestFrame=nullptr;bool dirty=false;};
static x11_xs_entry* entry=nullptr;
x11_xs_entry* entry_of_window(uint32_t){return entry;}
void wlr_texture_destroy(void*){assert(queueAlive);++releases;}
void wlr_buffer_unlock(void*){assert(queueAlive);++releases;}
void x11_toplevel_clear_guest_frame(uint32_t,uint64_t);
struct OH_NativeImage {bool alive=true;};
struct Binding {uint32_t surfaceId=9;uint64_t generation=1,surfaceKey=8;OH_NativeImage* image;
 void* producerWindow=nullptr;uint32_t clientPid=7;uint64_t frames=1;};
struct GraphicsBroker {
 static GraphicsBroker& GetInstance(){static GraphicsBroker b;return b;}
 void SetZeroCopySurfaceReady(uint64_t,bool){}
 void DetachZeroCopyTarget(uint64_t){}
};
void OH_NativeImage_Destroy(OH_NativeImage** image){
 assert(*image&&(*image)->alive);assert(!child);
 (*image)->alive=false;*image=nullptr;queueAlive=false;
}
#define OH_LOG_INFO(...) ((void)0)
"""
main=r"""
int main(){
 Scene scene;g_out.scene=&scene;wlr_surface surface;wlr_xwayland_surface xs{&surface};
 for(int i=0;i<1000;++i){
  ohos_client_surface c{};c.xs=&xs;
  ClientAssociate(&c.associate,nullptr);assert(c.scene_surf&&c.scene_node_destroy.notify);
  auto* first=c.scene_surf;
  // A duplicate association removes the old listener; old destruction cannot clear the new node.
  ClientAssociate(&c.associate,nullptr);auto* second=c.scene_surf;DestroyScene(first);
  assert(c.scene_surf==second);DestroyScene(second);assert(!c.scene_surf);
  ClientAssociate(&c.associate,nullptr);auto* third=c.scene_surf;
  ClientDissociate(&c.dissociate,nullptr);assert(!c.scene_surf);DestroyScene(third);
 }
 failCreate=true;ohos_client_surface failed{};failed.xs=&xs;
 ClientAssociate(&failed.associate,nullptr);assert(!failed.scene_surf);failCreate=false;
 // Retained frames retire even after the X record has disappeared. New identities survive.
 wlr_scene_buffer node{};ohos_child_face f{1,&node};child=&f;
 wl_ohos_output_client_frame_retire(9,2);assert(child&&retired==0);
 OH_NativeImage image;Binding b{};b.image=&image;
 DropBinding(b,"X window already destroyed");assert(retired==1&&!image.alive&&!b.image);
 queueAlive=true;ohos_client_surface c{};c.generation=2;c.frame_node=&node;top=&c;
 wl_ohos_output_client_frame_retire(9,1);assert(releases==0);
 wl_ohos_output_client_frame_retire(9,2);assert(releases==1&&!node.node.enabled);
 top=nullptr;g_out.multiwindow=true;x11_xs_entry e{};e.guestTexture=&node;e.guestFrame=&node;entry=&e;
 wl_ohos_output_client_frame_retire(9,4);assert(releases==1&&e.guestFrame);
 wl_ohos_output_client_frame_retire(9,5);assert(releases==3&&!e.guestFrame&&!e.guestTexture&&e.dirty);
 wl_ohos_output_client_frame_retire(9,5);assert(releases==3);
 std::cout<<"PASS 1000 production scene associate/destroy cycles, duplicate/failure/dissociate, dead/reused XID retirement before queue destruction and multiwindow release\n";
}
"""
output='entry/src/main/cpp/display/ohos_output.c'
source=prelude+function(output,'static void ClientSceneNodeDestroy(')+function(output,'static void ClientAssociate(')+function(output,'static void ClientDissociate(')+function('entry/src/main/cpp/display/x11_toplevel.c','void x11_toplevel_clear_guest_frame(')+function(output,'void wl_ohos_output_client_frame_retire(')+function('entry/src/main/cpp/display/display_guest_frames.cpp','void DropBinding(')+main
cpp=BUILD/'probe.cpp';cpp.write_text(source)
flags=shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','wayland-server'],text=True))
subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-pthread',str(cpp),'-o',str(BUILD/'probe'),*flags],check=True)
subprocess.run([str(BUILD/'probe')],check=True,timeout=10)
