#!/usr/bin/env python3
"""Exercise production X11 GPU child geometry without any new game frames."""
from pathlib import Path
import subprocess
ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'build/host_tests/x11-child-geometry'
BUILD.mkdir(parents=True, exist_ok=True)
s = (ROOT / 'entry/src/main/cpp/display/ohos_output.c').read_text()
a = s.index('static bool ChildFaceSyncGeometry(')
b = s.index('\n}\n', a) + 2
source = r"""
#include <cassert>
#include <cstdint>
#include <iostream>
struct wlr_scene_node {int x=0,y=0;};
struct wlr_scene_buffer {wlr_scene_node node;int w=0,h=0;};
struct ohos_child_face {uint32_t xwindow;uint64_t generation;
 wlr_scene_buffer* frame_node;int region_x,region_y,region_w,region_h;};
int qx=10,qy=20,qw=960,qh=640;uint64_t qg=7;bool live=true;
int ChildClientAreaQuery(uint32_t id,int* x,int* y,int* w,int* h,uint64_t* g){
 assert(id==31);if(!live)return 0;*x=qx;*y=qy;*w=qw;*h=qh;*g=qg;return 1;
}
void ChildFaceSyncClip(ohos_child_face*,int,int){}
void wlr_scene_node_set_position(wlr_scene_node* n,int x,int y){n->x=x;n->y=y;}
void wlr_scene_buffer_set_dest_size(wlr_scene_buffer* n,int w,int h){n->w=w;n->h=h;}
""" + s[a:b] + r"""
int main(){
 wlr_scene_buffer node;ohos_child_face f{31,7,&node,0,0,0,0};
 assert(ChildFaceSyncGeometry(&f)&&node.node.x==10&&node.node.y==20&&node.w==960);
 // Modal move pauses game rendering: geometry still moves the retained frame.
 qx=170;qy=80;assert(ChildFaceSyncGeometry(&f));
 assert(node.node.x==170&&node.node.y==80&&node.h==640);
 // Resize and drawable subregion use the same client-area coordinate system.
 qw=800;qh=600;assert(ChildFaceSyncGeometry(&f)&&node.w==800&&node.h==600);
 f.region_x=4;f.region_y=8;f.region_w=790;f.region_h=580;
 assert(ChildFaceSyncGeometry(&f)&&node.node.x==174&&node.node.y==88&&node.w==790&&node.h==580);
 // Recycled XIDs and destroyed children cannot move a previous session's frame.
 ++qg;qx=999;assert(!ChildFaceSyncGeometry(&f)&&node.node.x==174);
 qg=7;live=false;assert(!ChildFaceSyncGeometry(&f));
 live=true;f.frame_node=nullptr;assert(ChildFaceSyncGeometry(&f));
 std::cout<<"PASS production X11 retained GPU frame follows modal move/resize, subregion and generation\n";
}
"""
cpp=BUILD/'probe.cpp';cpp.write_text(source)
subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',str(cpp),'-o',str(BUILD/'probe')],check=True)
subprocess.run([str(BUILD/'probe')],check=True,timeout=10)
