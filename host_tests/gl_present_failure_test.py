"""Run the production GL Present/Attach methods with instrumented EGL/GL."""
from pathlib import Path
import subprocess,tempfile,re
R=Path(__file__).resolve().parents[1]
s=(R/'entry/src/main/cpp/graphics/virgl_surface_presenter.cpp').read_text()
def method(marker):
 start=s.index(marker);opening=s.index('{',start);depth=1;end=opening+1
 while depth:depth+=(s[end]=='{')-(s[end]=='}');end+=1
 return s[start:end].replace(' override','')
methods='\n'.join(method(x) for x in ['    int Attach(uint64_t','    int Present(GLuint'])
constants=sorted(set(re.findall(r'\b(?:GL|EGL)_[A-Z0-9_]+\b',methods)))
codes=sorted(set(re.findall(r'\bkPresent\w+',methods)))
code=r"""
#include <cassert>
#include <cstdint>
#include <mutex>
#include <cstdio>
#include "graphics/present_pacing.h"
#include "hilog/log.h"
using GLenum=unsigned;using GLuint=unsigned;using GLint=int;using GLsizei=int;using GLsync=void*;
using EGLDisplay=int;using EGLContext=int;using EGLSurface=int;using EGLBoolean=int;using EGLint=int;
struct OHNativeWindow {};
static uint64_t clockNs=1000000000;
static int mode=0,fences=0,swaps=0,makeCalls=0;
static uint64_t NowNs(){return clockNs;}
static bool PresentPerfSummaryEnabled(){return false;}
static uint64_t NormalizeVirglFramePeriodNs(uint64_t n){return winehua::NormalizePresentFramePeriodNs(n);}
static uint64_t VirglQueuePacingPeriodNs(uint64_t n){return winehua::QueuePresentPacingPeriodNs(n);}
namespace winehua {enum class NativeWindowReleaseMode{UnreferenceNativeObject,DestroyParcelWindow};}
struct Lease{OHNativeWindow* window=nullptr;explicit operator bool()const{return window;}OHNativeWindow* Get(){return window;}
 void Adopt(OHNativeWindow *w,winehua::NativeWindowReleaseMode){window=w;}};
"""
for n in constants:
 value=0 if n in ['GL_NO_ERROR','EGL_NO_DISPLAY','EGL_NO_CONTEXT','EGL_FALSE'] else 1
 code+=f'constexpr int {n}={value};\n'
code+='enum {'+','.join(codes)+'};\nconstexpr int SET_UI_TIMESTAMP=1;\n'
code+=r"""
static EGLDisplay eglGetCurrentDisplay(){return 5;}
static EGLContext eglGetCurrentContext(){return 5;}
static EGLSurface eglGetCurrentSurface(int){return 5;}
static int glIsTexture(unsigned){return mode==6?0:GL_TRUE;}
static GLsync glFenceSync(int,int){++fences;return mode==1?nullptr:(void*)1;}
static void glFlush(){}
static void glFinish(){}
static void glDeleteSync(GLsync){}
static EGLBoolean eglMakeCurrent(EGLDisplay d,EGLSurface,EGLSurface,EGLContext){++makeCalls;return ((mode==3&&d==6)||(mode==5&&d==5))?EGL_FALSE:EGL_TRUE;}
static void glWaitSync(GLsync,int,int){}
static void glViewport(int,int,int,int){}
static void glDisable(int){}
static void glUseProgram(int){}
static void glActiveTexture(int){}
static void glBindTexture(int,int){}
static void glBindSampler(int,int){}
static void glUniform1i(int,int){}
static int OH_NativeWindow_NativeWindowHandleOpt(OHNativeWindow*,int,uint64_t){return 0;}
static void glDrawArrays(int,int,int){}
static GLenum glGetError(){return mode==4?99:GL_NO_ERROR;}
static EGLBoolean eglSwapBuffers(int,int){++swaps;return mode==7?EGL_FALSE:EGL_TRUE;}
static EGLint eglGetError(){return 99;}
struct Target {
 std::mutex mutex_;Lease windowLease_;uint64_t surfaceKey_=0,framePeriodNs_=16666667,displayPeriodNs_=16666667;
 uint64_t frames_=0,failures_=0,timestampFailures_=0,throttled_=0,lastPresentNs_=0;
 uint32_t width_=0,height_=0;int display_=6,surface_=6,context_=6,program_=1,sampler_=1,textureLocation_=0;
 winehua::GlPresentFailureBackoff failureBackoff_;
 void ResetGlLocked(){width_=height_=0;}
 bool EnsureGlLocked(EGLDisplay,EGLContext,uint32_t w,uint32_t h){if(mode==2)return false;width_=w;height_=h;return true;}
"""+methods+r"""
};
int main(){
 unsigned checks=0;OHNativeWindow window;
 for(int fault:{1,2,3,4,5,7}){
  Target t;t.Attach(1,16666667,&window,false);mode=fault;uint64_t retry=0;
  int result=t.Present(1,320,200,1,1,&retry);assert(result!=kPresentOk&&retry>clockNs);++checks;
  int oldFences=fences,oldSwaps=swaps;
  assert(t.Present(1,320,200,1,2,&retry)==kPresentThrottled&&fences==oldFences&&swaps==oldSwaps);++checks;
  clockNs=retry;mode=0;assert(t.Present(1,320,200,1,3,&retry)==kPresentOk);++checks;
  clockNs+=100000000;mode=fault;
  assert(t.Present(1,640,480,1,4,&retry)!=kPresentOk&&retry-clockNs==16666667);++checks;
  // Attach cancels previous target's failure even without waiting its deadline.
  mode=0;t.Attach(2,16666667,&window,false);
  assert(t.Present(1,640,480,2,1,&retry)==kPresentOk);++checks;
 }
 Target t;t.Attach(3,16666667,&window,false);mode=1;uint64_t retry;
 for(int i=0;i<10;++i){t.Present(1,320,200,3,i,&retry);assert(retry>clockNs&&retry-clockNs<=50000000);clockNs=retry;++checks;}
 mode=0;assert(t.Present(1,320,200,3,99,&retry)==kPresentOk);++checks;
 const int oldFences=fences;assert(t.Present(1,320,200,3,100,&retry)==kPresentThrottled&&fences==oldFences);++checks;
 mode=6;assert(t.Present(1,320,200,3,100,&retry)==kPresentSourceInvisible);++checks;
 printf("GL production Present: %u fault/recovery checks PASS\n",checks);
}
"""
with tempfile.TemporaryDirectory(prefix='gl-present-') as d:
 d=Path(d);(d/'test.cpp').write_text(code)
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-I',str(R/'host_tests/stubs'),'-I',str(R/'entry/src/main/cpp'),str(d/'test.cpp'),'-o',str(d/'test')],check=True)
 subprocess.run([str(d/'test')],check=True)
