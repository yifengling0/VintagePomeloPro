#!/usr/bin/env python3
"""Exercise production XWM parser/cache and scene clipping with real pixman."""
from pathlib import Path
import subprocess
R=Path(__file__).resolve().parents[1]
B=R/'build/host_tests/x11-visible-region';B.mkdir(parents=True,exist_ok=True)
def function(s,name):
 a=s.index(name);a=s.rfind('\n',0,a)+1
 b=s.index('\n}',a)+2
 return s[a:b]
x=(R/'thirdparty/wlroots/xwayland/xwm.c').read_text()
scene=(R/'thirdparty/wlroots/types/scene/wlr_scene.c').read_text()
source=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <pixman.h>
typedef uint32_t xcb_window_t;
typedef unsigned xcb_get_property_cookie_t;
typedef struct {uint32_t type,format,bytes_after;int len;uint32_t data[4100];} xcb_get_property_reply_t;
enum{WINEHUA_GPU_VISIBLE_REGION,WINEHUA_GPU_CLIP_REQUEST,XCB_ATOM_CARDINAL=6,XCB_PROP_MODE_REPLACE=0};
struct xwm_child_window{uint32_t window_id;uint64_t generation;bool clip_requested,clip_valid,mapped;
 uint32_t clip_width,clip_height,width,height;pixman_region32_t visible_region;};
struct wlr_xwm{void*xcb_conn;uint32_t atoms[2];struct xwm_child_window child;};
struct wlr_xwayland{struct wlr_xwm*xwm;};
static xcb_get_property_reply_t payload;static int reads,writes;
static xcb_get_property_cookie_t xcb_get_property(void*a,int b,uint32_t c,uint32_t d,int e,int f,int g){
 (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;assert(g==4100);++reads;return 0;}
static xcb_get_property_reply_t*xcb_get_property_reply(void*a,unsigned b,void*c){
 (void)a;(void)b;(void)c;void*r=malloc(sizeof(payload));memcpy(r,&payload,sizeof(payload));return r;}
static int xcb_get_property_value_length(xcb_get_property_reply_t*r){return r->len;}
static void*xcb_get_property_value(xcb_get_property_reply_t*r){return r->data;}
static void xcb_change_property(void*a,int b,uint32_t c,uint32_t d,int e,int f,int g,const void*h){
 (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;assert(*(const uint32_t*)h==1);++writes;}
static struct xwm_child_window*xwm_child_window_find(struct wlr_xwm*x,uint32_t id){return x->child.window_id==id?&x->child:NULL;}
'''
source+=function(x,'static void xwm_child_read_visible_region(')+'\n'
source+=function(x,'bool wlr_xwayland_query_child_visible_region(')+'\n'
source+=r'''
enum {WLR_SCENE_NODE_BUFFER=2};
struct wlr_box {int x,y,width,height;};
struct wlr_scene_node {int type;pixman_region32_t visible;};
struct wlr_scene_buffer {struct wlr_scene_node node;bool clip_enabled;pixman_region32_t clip_region;};
struct scene_update_data {pixman_region32_t*update_region,*visible;bool calculate_visibility,restack_xwayland_surfaces;void*outputs;};
static struct wlr_scene_buffer*wlr_scene_buffer_from_node(struct wlr_scene_node*n){return (void*)n;}
static void scene_node_get_size(struct wlr_scene_node*n,int*w,int*h){(void)n;*w=800;*h=600;}
static void scene_node_opaque_region(struct wlr_scene_node*n,int x,int y,pixman_region32_t*r){(void)n;pixman_region32_union_rect(r,r,x,y,800,600);}
static void update_node_update_outputs(struct wlr_scene_node*a,void*b,void*c,void*d){(void)a;(void)b;(void)c;(void)d;}
static int updates;
static void scene_node_update(struct wlr_scene_node*n,void*v){(void)n;(void)v;++updates;}
'''
source+=function(scene,'static bool scene_node_update_iterator(')+'\n'
source+=function(scene,'void wlr_scene_buffer_set_clip_region(')+'\n'
source+=r'''
static bool contains(pixman_region32_t*r,int x,int y){return pixman_region32_contains_point(r,x,y,NULL);}
int main(void){
 struct wlr_xwm xwm={0};struct wlr_xwayland xw={&xwm};struct xwm_child_window*c=&xwm.child;
 c->window_id=31;c->generation=7;c->mapped=true;c->width=800;c->height=600;pixman_region32_init(&c->visible_region);
 payload.type=XCB_ATOM_CARDINAL;payload.format=32;payload.len=32;
 uint32_t values[]={1,800,600,1,0,0,800,600};memcpy(payload.data,values,sizeof(values));
 pixman_region32_t r;pixman_region32_init(&r);
 assert(wlr_xwayland_query_child_visible_region(&xw,31,7,&r));assert(writes==1&&reads==1&&contains(&r,799,599));
 for(int i=0;i<10000;i++)assert(wlr_xwayland_query_child_visible_region(&xw,31,7,&r));
 assert(reads==1&&writes==1); // no synchronous X request on the frame path
 assert(!wlr_xwayland_query_child_visible_region(&xw,31,8,&r));
 c->mapped=false;assert(!wlr_xwayland_query_child_visible_region(&xw,31,7,&r));c->mapped=true;
 c->width=801;assert(!wlr_xwayland_query_child_visible_region(&xw,31,7,&r));c->width=800;
 // Window/menu obscures the center: use real disjoint server rectangles.
 uint32_t hole[]={1,800,600,4,0,0,800,100,0,100,200,500,600,100,800,500,0,500,800,600};
 memcpy(payload.data,hole,sizeof(hole));payload.len=sizeof(hole);xwm_child_read_visible_region(&xwm,c);
 assert(c->clip_valid&&!contains(&c->visible_region,400,300)&&contains(&c->visible_region,50,300));
 // Truncated, trailing, wrong version, inverted and out-of-range rectangles fail closed.
 payload.bytes_after=4;xwm_child_read_visible_region(&xwm,c);assert(!c->clip_valid);payload.bytes_after=0;
 --payload.len;xwm_child_read_visible_region(&xwm,c);assert(!c->clip_valid);++payload.len;
 payload.data[0]=2;xwm_child_read_visible_region(&xwm,c);assert(!c->clip_valid);payload.data[0]=1;
 payload.data[6]=0;xwm_child_read_visible_region(&xwm,c);assert(!c->clip_valid);payload.data[6]=800;
 payload.data[6]=0x7fffffff;xwm_child_read_visible_region(&xwm,c);assert(!c->clip_valid);payload.data[6]=800;
 payload.data[3]=1025;xwm_child_read_visible_region(&xwm,c);assert(!c->clip_valid);payload.data[3]=4;
 xwm_child_read_visible_region(&xwm,c);assert(c->clip_valid);
 // Scene visibility and opaque culling both honor the hole, with local->root translation.
 struct wlr_scene_buffer b={.node.type=WLR_SCENE_NODE_BUFFER};
 pixman_region32_init(&b.node.visible);pixman_region32_init(&b.clip_region);
 wlr_scene_buffer_set_clip_region(&b,&c->visible_region);assert(updates==1);
 wlr_scene_buffer_set_clip_region(&b,&c->visible_region);assert(updates==1);
 pixman_region32_t available,update;pixman_region32_init_rect(&available,0,0,2000,1600);
 pixman_region32_init_rect(&update,0,0,2000,1600);
 struct scene_update_data d={&update,&available,true,false,NULL};
 scene_node_update_iterator(&b.node,100,200,&d);
 assert(!contains(&b.node.visible,500,500)&&contains(&b.node.visible,150,500));
 assert(contains(&available,500,500)&&!contains(&available,150,500)); // GDI beneath is not culled
 // Completely covered is deliberately empty, not 'disable clipping'.
 pixman_region32_clear(&r);wlr_scene_buffer_set_clip_region(&b,&r);assert(updates==2);
 pixman_region32_clear(&b.node.visible);pixman_region32_union_rect(&available,&available,0,0,2000,1600);
 scene_node_update_iterator(&b.node,100,200,&d);assert(pixman_region32_empty(&b.node.visible));
 wlr_scene_buffer_set_clip_region(&b,NULL);assert(!b.clip_enabled&&updates==3);
 pixman_region32_fini(&b.node.visible);pixman_region32_fini(&b.clip_region);
 pixman_region32_fini(&available);pixman_region32_fini(&update);pixman_region32_fini(&c->visible_region);pixman_region32_fini(&r);
 puts("PASS production visible-region parser/cache, 10000 request-free queries, holes, opaque culling, incarnation, malformed/empty regions");
}
'''
p=B/'probe.c';p.write_text(source)
flags=subprocess.check_output(['pkg-config','--cflags','--libs','pixman-1'],text=True).split()
subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p),'-o',str(B/'probe'),*flags],check=True)
subprocess.run([str(B/'probe')],check=True,timeout=20)

# Exercise the actual X server producer, using its validated borderClip and
# deliberately making ClipShape/own-client-content different from it.
xs=(R/'thirdparty/xserver/hw/xwayland/xwayland-window.c').read_text()
publisher='void\n'+function(xs,'xwl_window_publish_gpu_visible_region(WindowPtr window)')
callback='static void\n'+function((R/'thirdparty/xserver/hw/xwayland/xwayland-screen.c').read_text(),'xwl_property_callback(')
producer=r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pixman.h>
typedef uint32_t CARD32;typedef int32_t INT32;
typedef pixman_box32_t*BoxPtr;
enum{Success=0,DixReadAccess=1,XA_CARDINAL=6,PropModeReplace=0,TRUE=1,PropertyNewValue=0,PropertyDelete=1};
struct property{int type,format,propertyName;size_t size;void*data;};typedef struct property*PropertyPtr;
struct window{struct{void*pScreen;int x,y,width,height;}drawable;bool viewable;pixman_region32_t borderClip;};
typedef struct window*WindowPtr;
struct xwl_screen{int gpu_clip_request_prop,gpu_visible_region_prop,allow_commits_prop;};
typedef void*ScreenPtr;typedef void*CallbackListPtr;
typedef struct{WindowPtr win;PropertyPtr prop;int state;}PropertyStateRec;
struct xwl_window{int dummy;};
static struct xwl_window*xwl_window_get(WindowPtr w){(void)w;return NULL;}
static void xwl_window_update_property(struct xwl_window*w,PropertyStateRec*r){(void)w;(void)r;assert(false);}
static struct xwl_screen xs={1,2,3};static int serverClient,events,lookups;static bool optin=true;
static CARD32 version=1;static struct property req={6,32,1,1,&version},out={0};
static struct xwl_screen*xwl_screen_get(void*v){(void)v;return &xs;}
static int dixLookupProperty(PropertyPtr*p,WindowPtr w,int atom,int c,int a){
 (void)w;(void)c;(void)a;++lookups;if(atom==1&&optin){*p=&req;return Success;}
 if(atom==2&&out.data){*p=&out;return Success;}return 1;
}
static int dixChangeWindowProperty(int c,WindowPtr w,int atom,int type,int format,int mode,size_t words,const void*data,int notify){
 (void)c;(void)w;(void)mode;assert(atom==2&&type==6&&format==32&&notify);
 free(out.data);out=(struct property){6,32,2,words,malloc(words*4)};assert(out.data);memcpy(out.data,data,words*4);++events;return 0;
}
static int RegionNumRects(pixman_region32_t*r){return pixman_region32_n_rects(r);}
static BoxPtr RegionRects(pixman_region32_t*r){return pixman_region32_rectangles(r,NULL);}
'''+publisher+'\n'+callback+r'''
int main(void){
 struct window w={.drawable={NULL,150,240,800,600},.viewable=true};
 pixman_region32_init_rect(&w.borderClip,150,240,800,600);
 PropertyStateRec deleted={&w,&req,PropertyDelete};
 xwl_property_callback(NULL,NULL,&deleted);assert(lookups==0&&events==0); // freed chain must not be traversed
 optin=false;xwl_window_publish_gpu_visible_region(&w);assert(events==0);optin=true;
 xwl_window_publish_gpu_visible_region(&w);assert(events==1&&out.size==8);
 CARD32*v=out.data;assert(v[0]==1&&v[1]==800&&v[2]==600&&v[3]==1);
 assert(v[4]==0&&v[5]==0&&v[6]==800&&v[7]==600);
 xwl_window_publish_gpu_visible_region(&w);assert(events==1); // no notification feedback loop
 pixman_region32_t hole;pixman_region32_init_rect(&hole,350,340,400,400);
 pixman_region32_subtract(&w.borderClip,&w.borderClip,&hole);
 xwl_window_publish_gpu_visible_region(&w);v=out.data;assert(events==2&&v[3]==4&&out.size==20);
 w.viewable=false; // mi's unmap ClipNotify can precede borderClip clearing
 xwl_window_publish_gpu_visible_region(&w);v=out.data;assert(events==3&&v[3]==0&&out.size==4);
 w.viewable=true;pixman_region32_clear(&w.borderClip);
 for(int i=0;i<1025;i++)pixman_region32_union_rect(&w.borderClip,&w.borderClip,i*2,0,1,1);
 xwl_window_publish_gpu_visible_region(&w);assert(events==3); // over-limit remains empty, never truncated
 pixman_region32_fini(&w.borderClip);pixman_region32_fini(&hole);free(out.data);
 puts("PASS production X server visible-region producer: opt-in, local coordinates, real occlusion, unmap, bounds and change-only publication");
}
'''
p=B/'producer.c';p.write_text(producer)
subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-fsanitize=address,undefined',str(p),'-o',str(B/'producer'),*flags],check=True)
subprocess.run([str(B/'producer')],check=True,timeout=20)
