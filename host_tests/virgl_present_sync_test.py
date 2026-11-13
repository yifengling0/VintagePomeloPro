"""Exercise production GL Present with independently scheduled shared contexts.

A successful swap only submits the presenter context. The producer must wait
for its sampling fence before it may overwrite the shared source texture.
"""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'entry/src/main/cpp/graphics/virgl_surface_presenter.cpp').read_text()

def method(marker):
    start = source.index(marker)
    opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end].replace(' override', '')

methods = '\n'.join(method(x) for x in ('    int Attach(uint64_t', '    int Present(GLuint'))
constants = sorted(set(re.findall(r'\b(?:GL|EGL)_[A-Z0-9_]+\b', methods)))
codes = sorted(set(re.findall(r'\bkPresent\w+', methods)))
code = r"""
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <vector>
#include "graphics/present_pacing.h"
#include "hilog/log.h"
using GLenum=unsigned; using GLuint=unsigned; using GLsizei=int; using GLint=int;
using EGLDisplay=int; using EGLContext=int; using EGLSurface=int; using EGLBoolean=int; using EGLint=int;
struct Sync { int context; bool sampling; bool deleted=false; };
using GLsync=Sync*;
struct OHNativeWindow {};
static uint64_t now=1000000000;
static int current=5, fault=0, producerWaits=0, consumerWaits=0, finishes=0;
static bool readPending=false, sourceSafe=true;
static std::vector<Sync> syncs;
static uint64_t NowNs(){return now;}
static bool PresentPerfSummaryEnabled(){return false;}
static uint64_t NormalizeVirglFramePeriodNs(uint64_t n){return winehua::NormalizePresentFramePeriodNs(n);}
static uint64_t VirglQueuePacingPeriodNs(uint64_t n){return winehua::QueuePresentPacingPeriodNs(n);}
namespace winehua { enum class NativeWindowReleaseMode { UnreferenceNativeObject, DestroyParcelWindow }; }
struct Lease { OHNativeWindow* value=nullptr; explicit operator bool()const{return value;}
OHNativeWindow* Get(){return value;} void Adopt(OHNativeWindow* p,winehua::NativeWindowReleaseMode){value=p;} };
"""
for name in constants:
    value = 0 if name in ('GL_NO_ERROR', 'EGL_NO_DISPLAY', 'EGL_NO_CONTEXT', 'EGL_FALSE') else 1
    code += f'constexpr int {name}={value};\n'
code += 'enum {' + ','.join(codes) + '};\nconstexpr int SET_UI_TIMESTAMP=1;\n'
code += r"""
static EGLDisplay eglGetCurrentDisplay(){return current;}
static EGLContext eglGetCurrentContext(){return current;}
static EGLSurface eglGetCurrentSurface(int){return current;}
static int glIsTexture(unsigned){return GL_TRUE;}
static GLsync glFenceSync(int,int){
 if(fault==2 && current==6)return nullptr;
 syncs.push_back({current,current==6&&readPending,false}); return &syncs.back();
}
static void glFlush(){}
[[maybe_unused]] static void glFinish(){assert(current==6);++finishes;readPending=false;sourceSafe=true;}
static void glDeleteSync(GLsync s){assert(s&&!s->deleted);s->deleted=true;}
static void glWaitSync(GLsync s,int,int){
 assert(s&&!s->deleted);
 if(current==6){assert(s->context==5);++consumerWaits;}
 else {assert(s->context==6&&s->sampling);++producerWaits;sourceSafe=true;}
}
static EGLBoolean eglMakeCurrent(EGLDisplay d,EGLSurface,EGLSurface,EGLContext){
 if(fault==3&&d==5)return EGL_FALSE;
 current=d;return EGL_TRUE;
}
static void glViewport(int,int,int,int){} static void glDisable(int){}
static void glUseProgram(int){} static void glActiveTexture(int){}
static void glBindTexture(int,int){} static void glBindSampler(int,int){}
static void glUniform1i(int,int){}
static int OH_NativeWindow_NativeWindowHandleOpt(OHNativeWindow*,int,uint64_t){return 0;}
static void glDrawArrays(int,int,int){assert(current==6);readPending=true;sourceSafe=false;}
static GLenum glGetError(){return GL_NO_ERROR;}
static EGLBoolean eglSwapBuffers(int,int){return fault==1?EGL_FALSE:EGL_TRUE;}
static EGLint eglGetError(){return 99;}
struct Target {
 std::mutex mutex_;Lease windowLease_;uint64_t surfaceKey_=0,framePeriodNs_=16666667,displayPeriodNs_=16666667;
 uint64_t frames_=0,failures_=0,timestampFailures_=0,throttled_=0,lastPresentNs_=0;
 uint32_t width_=0,height_=0;int display_=6,surface_=6,context_=6,program_=1,sampler_=1,textureLocation_=0;
 winehua::GlPresentFailureBackoff failureBackoff_;
 void ResetGlLocked(){width_=height_=0;}
 bool EnsureGlLocked(EGLDisplay,EGLContext,uint32_t w,uint32_t h){width_=w;height_=h;return true;}
""" + methods + r"""
};
int main(){
 syncs.reserve(128); OHNativeWindow window; unsigned checks=0;
 for(int scenario : {0,1,2,3}) {
  Target t;current=5;fault=scenario;readPending=false;sourceSafe=true;
  int oldProducer=producerWaits,oldConsumer=consumerWaits,oldFinish=finishes;
  t.Attach(1,16666667,&window,false);uint64_t retry=0;
  int result=t.Present(1,320,200,1,1,&retry);
  assert((scenario==0)==(result==kPresentOk));++checks;
  // Model the producer's first overwrite after Present returns. Swap/flush
  // alone must not be treated as completion of the independent read queue.
  assert(sourceSafe&&"shared source reused before presenter read was ordered");++checks;
  assert(consumerWaits==oldConsumer+1);++checks;
  if(scenario==0||scenario==1){assert(producerWaits==oldProducer+1&&finishes==oldFinish);++checks;}
  else {assert(finishes==oldFinish+1);++checks;}
  for(const auto& s:syncs){assert(s.deleted);++checks;}
  now+=100000000;
 }
 printf("GL shared texture Present: %u bidirectional ordering/failure checks PASS\n",checks);
}
"""
with tempfile.TemporaryDirectory(prefix='virgl-read-order-') as folder:
    folder = Path(folder)
    (folder / 'test.cpp').write_text(code)
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
        '-I', str(ROOT / 'host_tests/stubs'), '-I', str(ROOT / 'entry/src/main/cpp'),
        str(folder / 'test.cpp'), '-o', str(folder / 'test')], check=True)
    subprocess.run([str(folder / 'test')], check=True)
