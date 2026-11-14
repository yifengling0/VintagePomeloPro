"""Fault injection into the production producer-surface recovery branch."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'entry/src/main/cpp/graphics/virgl_surface_presenter.cpp').read_text()
start = source.index('    bool EnsureGlLocked(')
end = source.index('        if (display_ == sourceDisplay', start)
# The recovery branch precedes normal initialization. Keep it verbatim and
# stop before unrelated shader/initial-context setup, which is never reached
# by recovery. Any attempted context/consumer teardown is a compile failure.
branch = source[start:end] + 'return surface_ != EGL_NO_SURFACE;\n}\n'
code = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include "graphics/surface_queue_recovery.h"
using EGLDisplay=int; using EGLContext=int; using EGLSurface=int;
using EGLNativeWindowType=void*;
constexpr int EGL_NO_CONTEXT=0, EGL_NO_SURFACE=0, SET_BUFFER_GEOMETRY=1;
static uint64_t now=1000000000;
static int destroyed=0, created=0, geometry=0, fault=0;
static uint64_t NowNs(){return now;}
static void eglDestroySurface(int display,int surface){
 assert(display==5&&surface==6);++destroyed;
}
static int OH_NativeWindow_NativeWindowHandleOpt(void* window,int op,int w,int h){
 assert(window&&op==SET_BUFFER_GEOMETRY&&w==800&&h==600);++geometry;
 return fault==1 ? -1 : 0;
}
static int eglCreateWindowSurface(int display,void* config,void* window,void*){
 assert(display==5&&config&&window);++created;return fault==2?0:9;
}
struct Lease {void* Get(){return this;}};
struct Target {
 int display_=5,sourceContext_=5,context_=7,surface_=6;
 int program_=22,sampler_=33;void* config_=this;
 uint32_t width_=320,height_=200; Lease windowLease_;
 winehua::SurfaceQueueRecovery recovery_;
''' + branch + r'''
};
int main(){
 for(int failure:{0,1,2}){
  Target t;fault=failure;destroyed=created=geometry=0;
  t.recovery_.Request(now);now+=winehua::kSurfaceRecoveryDelayNs;
  const bool ok=t.EnsureGlLocked(5,5,800,600);
  assert(ok==(failure==0));assert(destroyed==1&&geometry==1);
  assert(created==(failure==1?0:1));
  assert(t.context_==7&&t.program_==22&&t.sampler_==33&&t.config_==&t);
  if(ok){assert(!t.recovery_.Pending()&&t.surface_==9&&t.width_==800&&t.height_==600);}
  else{
   assert(t.surface_==0&&t.recovery_.Deferred(now));
   assert(t.width_==320&&t.height_==200);
   now=t.recovery_.RetryAfterNs();fault=0;destroyed=created=geometry=0;
   assert(t.EnsureGlLocked(5,5,800,600));
   assert(destroyed==0&&created==1&&!t.recovery_.Pending());
  }
 }
 Target t;t.recovery_.Request(now);destroyed=created=geometry=0;
 assert(t.EnsureGlLocked(8,8,800,600));
 assert(destroyed==0&&created==0&&geometry==0&&t.recovery_.Pending());
 puts("Production EGL recovery: rebuild/failure/retry/context ownership PASS");
}
'''
code = code.replace('#include <cstdio>', '#include <cstdio>\n#include <initializer_list>')
with tempfile.TemporaryDirectory(prefix='surface-rebuild-') as temporary:
    folder = Path(temporary)
    (folder / 'test.cpp').write_text(code)
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
        '-I', str(ROOT / 'entry/src/main/cpp'), str(folder / 'test.cpp'),
        '-o', str(folder / 'test')], check=True)
    subprocess.run([str(folder / 'test')], check=True)
