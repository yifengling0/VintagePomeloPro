#!/usr/bin/env python3
"""Execute the production private EGL surface constructor in a no-GLX host."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'thirdparty/wine-valve/dlls/winex11.drv/opengl_winehua.c').read_text()
a=s.index('static BOOL winehua_x11_surface_create(');b=s.index('\nstatic void winehua_x11_init_egl_platform',a)
production=s[a:b]
test=r"""
#include <cassert>
#include <cstdlib>
#include <algorithm>
using BOOL=int;using HWND=int;using EGLint=int;using EGLSurface=int;using EGLConfig=int;
constexpr int FALSE=0,TRUE=1,EGL_WIDTH=10,EGL_HEIGHT=11,EGL_NONE=0;
constexpr int GL_FRONT_LEFT=1,GL_BACK_LEFT=2,GL_FRONT=3,GL_BACK=4,GL_FRONT_AND_BACK=5,GL_FRONT_RIGHT=6,GL_BACK_RIGHT=7;
#define ERR(...) ((void)0)
#define WARN(...) ((void)0)
#define TRACE(...) ((void)0)
using std::max;
struct RECT {int left,top,right,bottom;};
struct client_surface {int refs=1;};
struct opengl_drawable {int format=0,stereo=0,surface=0;client_surface* client=nullptr;};
struct winehua_x11_gl_drawable {opengl_drawable base;int width,height;};
static int winehua_x11_drawable_funcs,visualRequests=0,requestedFormat=-1,lastConfig=0,created=0;
static bool failClient=false,failPbuffer=false,zeroHeight=false;static client_surface client;
static struct {int display=99;} platform;
static auto* egl_platform=&platform;
int x11drv_client_surface_create(int,BOOL,int format,client_surface** p){
 ++visualRequests;requestedFormat=format;
 // In no-GLX builds the real visual_from_pixel_format rejects every nonzero format.
 if(format||failClient)return 0;
 ++client.refs;*p=&client;return 42;
}
void winehua_present_surface_init(){}
bool winehua_present_surface_mapped(){return true;}
void client_surface_release(client_surface* p){--p->refs;}
winehua_x11_gl_drawable* opengl_drawable_create(size_t n,void*,int f,client_surface* c){
 auto* p=(winehua_x11_gl_drawable*)calloc(1,n);p->base.format=f;p->base.client=c;++c->refs;return p;
}
void opengl_drawable_release(opengl_drawable* p){--p->client->refs;free(p);}
int NtUserGetDpiForWindow(int){return 96;}
void NtUserGetClientRect(int,RECT* p,int){*p={0,0,115,zeroHeight?0:80};}
int wx11_config_for_format(int f){assert(f>0);lastConfig=f;return 7;}
int createPbuffer(int display,int config,const int* attrs){
 assert(display==99&&config==7&&attrs[0]==EGL_WIDTH&&attrs[1]==115);
 assert(attrs[2]==EGL_HEIGHT&&attrs[3]==(zeroHeight?1:80)&&attrs[4]==EGL_NONE);
 ++created;return failPbuffer?0:66;
}
int getError(){return 0;}
static struct {int(*p_eglCreatePbufferSurface)(int,int,const int*)=createPbuffer;int(*p_eglGetError)()=getError;} functions;
static auto* funcs=&functions;
void opengl_drawable_map_buffer(opengl_drawable*,int,int){}
PRODUCTION
int main(){
 for(int format:{1,87,168}){
  opengl_drawable* d=nullptr;int before=client.refs;
  assert(winehua_x11_surface_create(5,FALSE,format,&d));
  assert(d&&requestedFormat==0&&lastConfig==format&&d->format==format&&d->surface==66);
  int previousCreates=created;
  assert(winehua_x11_surface_create(5,FALSE,format,&d)&&created==previousCreates);
  opengl_drawable_release(d);assert(client.refs==before);
 }
 // WineD3D's hidden probe can have zero client height. Pbuffers must stay nonzero.
 zeroHeight=true;opengl_drawable* d=nullptr;
 assert(winehua_x11_surface_create(5,FALSE,87,&d));opengl_drawable_release(d);
 int before=client.refs;d=nullptr;failPbuffer=true;
 assert(!winehua_x11_surface_create(5,FALSE,87,&d)&&!d&&client.refs==before);
 failPbuffer=false;failClient=true;
 assert(!winehua_x11_surface_create(5,FALSE,87,&d)&&!d&&client.refs==before);
}
""".replace('PRODUCTION',production)
with tempfile.TemporaryDirectory(prefix='x11-egl-visual-') as d:
 p=Path(d);(p/'test.cpp').write_text(test)
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS production X11 EGL constructor: no-GLX visual, retained EGL format, hidden probe, reuse and failure cleanup')
