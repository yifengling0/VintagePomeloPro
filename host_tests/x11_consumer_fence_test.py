#!/usr/bin/env python3
"""Run production EGL export and queue-release code with native API fakes."""
from pathlib import Path
import subprocess
ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'build/host_tests/x11-consumer-fence'
BUILD.mkdir(parents=True, exist_ok=True)
def function(path, signature):
    source = (ROOT / path).read_text()
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]
prelude = r"""
#include <cassert>
#include <cstdint>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <iostream>
using EGLImageKHR=int; using EGLClientBuffer=void*;
struct OHNativeWindowBuffer {};
constexpr int EGL_NATIVE_BUFFER_OHOS=0x34e1,EGL_IMAGE_PRESERVED_KHR=0x30d2;
static EGLClientBuffer lastImageBuffer=nullptr;static bool imageFails=false;
using EGLDisplay=int; using EGLContext=int; using EGLSurface=int;
using EGLint=int; using EGLenum=int; using EGLSyncKHR=int;
using GLenum=unsigned;
constexpr GLenum GL_NO_ERROR=0,GL_OUT_OF_MEMORY=0x505;
static unsigned pendingErrorCount=0,errorChecks=0;
GLenum glGetError(){++errorChecks; if(!pendingErrorCount)return GL_NO_ERROR;--pendingErrorCount;return GL_OUT_OF_MEMORY;}
constexpr int EGL_NO_DISPLAY=0,EGL_NO_CONTEXT=0,EGL_NO_SURFACE=0,EGL_NO_SYNC_KHR=0;
constexpr int EGL_DRAW=1,EGL_READ=2,EGL_SYNC_NATIVE_FENCE_ANDROID=3;
constexpr int EGL_SYNC_NATIVE_FENCE_FD_ANDROID=4,EGL_NONE=0,EGL_TRUE=1;
using PFNEGLWAITSYNCKHRPROC=int(*)(EGLDisplay,EGLSyncKHR,EGLint);
using PFNEGLCREATESYNCKHRPROC=EGLSyncKHR(*)(EGLDisplay,EGLenum,const EGLint*);
using PFNEGLDESTROYSYNCKHRPROC=int(*)(EGLDisplay,EGLSyncKHR);
using PFNEGLDUPNATIVEFENCEFDANDROIDPROC=int(*)(EGLDisplay,EGLSyncKHR);
static void* g_renderer=(void*)1;
static int g_display=10,g_context=20,currentDisplay=0,currentContext=0,draw=0,readSurface=0;
static uint64_t g_eglSyncCount=0;
int CreateImage(int display,int context,int target,void* buffer,const int* attrs){
 assert(display==g_display&&context==EGL_NO_CONTEXT&&target==EGL_NATIVE_BUFFER_OHOS);
 assert(attrs&&attrs[0]==EGL_IMAGE_PRESERVED_KHR&&attrs[1]==EGL_TRUE&&attrs[2]==EGL_NONE);
 lastImageBuffer=buffer;return imageFails?0:99;
}
static auto g_create_image=&CreateImage;

static int flushes=0,finishes=0,errors=0,created=0,destroyed=0,exports=0;
static bool entrypoints=true,createFails=false,exportFails=false,waitFails=false,bindFails=false;
static int importedFd=-1,waits=0;
template<typename... Args> void LogError(const char*, Args...){++errors;}
#define TAG_ERR(...) LogError(__VA_ARGS__)
#define TAG_INFO(...) ((void)0)
int eglGetCurrentDisplay(){return currentDisplay;}
int eglGetCurrentContext(){return currentContext;}
int eglGetCurrentSurface(int s){return s==EGL_DRAW?draw:readSurface;}
int eglMakeCurrent(int d,int a,int b,int c){if(bindFails)return 0;currentDisplay=d;draw=a;readSurface=b;currentContext=c;return 1;}
int createSync(int d,int,const int* attrs){
 assert(d==g_display&&currentContext==g_context);++created;
 if(attrs){assert(attrs[0]==EGL_SYNC_NATIVE_FENCE_FD_ANDROID&&attrs[2]==EGL_NONE);
  importedFd=attrs[1];assert(fcntl(importedFd,F_GETFD)>=0);if(!createFails)close(importedFd);}
 return createFails?0:7;
}
int waitSync(int d,int s,int flags){
 assert(d==g_display&&s==7&&flags==0&&currentContext==g_context);++waits;return !waitFails;
}
int destroySync(int,int s){assert(s==7);++destroyed;return 1;}
int dupFence(int,int s){assert(s==7&&flushes>exports);++exports;return exportFails?-1:open("/dev/null",O_RDONLY);}
void glFlush(){++flushes;}
void glFinish(){assert(currentContext==g_context);++finishes;}
void* eglGetProcAddress(const char* n){
 if(!entrypoints)return nullptr;
 if(!strcmp(n,"eglCreateSyncKHR"))return (void*)&createSync;
 if(!strcmp(n,"eglDestroySyncKHR"))return (void*)&destroySync;
 if(!strcmp(n,"eglDupNativeFenceFDANDROID"))return (void*)&dupFence;
 if(!strcmp(n,"eglWaitSyncKHR"))return (void*)&waitSync;
 assert(false);return nullptr;
}
uint64_t wl_ohos_egl_sync_count(){return g_eglSyncCount;}
uint32_t wl_ohos_output_present_seq(){return 11;}
struct WlOhosConsumerBuffer {bool returned=false,handoff_valid=false;
 uint32_t handoff_present_seq=10;uint64_t handoff_sync_count=0;
 void* image=(void*)1;void* window_buffer=(void*)2;};
uint64_t g_relTotal=0,g_relSynced=0,g_relViolations=0,g_relUnrendered=0;
constexpr int WLR_ERROR=1;
void wlr_log(int,const char*,...){++errors;}
int queueCalls=0,queueFence=-1;
int OH_NativeImage_ReleaseNativeWindowBuffer(void*,void*,int fd){
 ++queueCalls;queueFence=fd;if(fd>=0)assert(close(fd)==0);return 0;
}
"""
main = r"""
int main(){
 // Native imports preserve existing pixels, including the final scene re-import.
 OHNativeWindowBuffer nativeImageBuffer;
 assert(ImportNativeBufferPreserved(&nativeImageBuffer)==99&&lastImageBuffer==&nativeImageBuffer);
 imageFails=true;assert(ImportNativeBufferPreserved(&nativeImageBuffer)==0);imageFails=false;
 // Lazy EGL draw-buffer acquisition must wait for ordinary backpressure, but
 // never inherit the SDK's three-second timeout on the shared event loop.
 assert(CanvasBufferWaitMs(8333333)==17&&CanvasBufferWaitMs(16666667)==34);
 assert(CanvasBufferWaitMs(33333333)==50&&CanvasBufferWaitMs(UINT64_MAX)==50);
 assert(CanvasBufferWaitMs(0)==1&&CanvasBufferWaitMs(1)==1);
 // Previous GL errors must be diagnosed before testing a new NativeBuffer
 // import. A stuck driver must not turn draining into an unbounded loop.
 pendingErrorCount=1;assert(WindowPresentGlErrors("previous")==GL_OUT_OF_MEMORY);
 assert(WindowPresentGlErrors("import")==GL_NO_ERROR);
 unsigned before=errorChecks;pendingErrorCount=100;
 assert(WindowPresentGlErrors("stuck")==GL_OUT_OF_MEMORY&&errorChecks-before==16);
 pendingErrorCount=0;errors=0;
 // Unbound and foreign contexts are both restored; export is asynchronous.
 int fd=wl_ohos_egl_export_release_fence();assert(fd>=0);close(fd);
 assert(finishes==0&&created==1&&destroyed==1&&currentContext==0&&draw==0);
 currentDisplay=33;currentContext=44;draw=55;readSurface=66;
 fd=wl_ohos_egl_export_release_fence();assert(fd>=0);close(fd);
 assert(currentDisplay==33&&currentContext==44&&draw==55&&readSurface==66);
 // Driver failures must finish the actual context before returning no fence.
 exportFails=true;assert(wl_ohos_egl_export_release_fence()==-1);
 assert(finishes==1&&g_eglSyncCount==1&&destroyed==3);
 exportFails=false;createFails=true;assert(wl_ohos_egl_export_release_fence()==-1);
 assert(finishes==2&&g_eglSyncCount==2&&destroyed==3);
 createFails=false;entrypoints=false;assert(wl_ohos_egl_export_release_fence()==-1);
 assert(finishes==3&&g_eglSyncCount==3);entrypoints=true;
 // Normal scene retirement consumes exactly one exported FD, no forced finish.
 WlOhosConsumerBuffer b;b.handoff_valid=true;b.handoff_sync_count=g_eglSyncCount;
 assert(ConsumerReleaseQueueSlot(&b,-1)==0&&queueFence>=0&&queueCalls==1);
 assert(fcntl(queueFence,F_GETFD)==-1&&finishes==3&&g_relSynced==1&&g_relViolations==0);
 assert(ConsumerReleaseQueueSlot(&b,-1)==-1&&queueCalls==1);
 // Unsubmitted frames need neither fence export nor global GPU stall.
 auto previousExports=exports;WlOhosConsumerBuffer unused;
 assert(ConsumerReleaseQueueSlot(&unused,-1)==0&&queueFence==-1);
 assert(exports==previousExports&&finishes==3);
 // Supplied fences are forwarded; fallback synchronization is also counted.
 WlOhosConsumerBuffer supplied;supplied.handoff_valid=true;
 fd=open("/dev/null",O_RDONLY);assert(ConsumerReleaseQueueSlot(&supplied,fd)==0);
 assert(exports==previousExports&&queueFence==fd&&fcntl(fd,F_GETFD)==-1);
 WlOhosConsumerBuffer fallback;fallback.handoff_valid=true;fallback.handoff_sync_count=g_eglSyncCount;
 entrypoints=false;assert(ConsumerReleaseQueueSlot(&fallback,-1)==0);
 assert(finishes==4&&g_relSynced==3&&g_relViolations==0&&errors==0);
 // Explicit producer completion must precede sampling, without CPU blocking.
 int pipefd[2];assert(pipe(pipefd)==0);entrypoints=true;int oldFinishes=finishes;
 assert(wl_ohos_egl_wait_acquire_fence(-1));
 assert(wl_ohos_egl_wait_acquire_fence(pipefd[0]));
 assert(waits==1&&finishes==oldFinishes&&fcntl(importedFd,F_GETFD)==-1);
 assert(fcntl(pipefd[0],F_GETFD)>=0&&currentDisplay==33&&currentContext==44&&draw==55&&readSurface==66);
 // Import failure closes only its duplicate; fallback preserves borrowed fd.
 createFails=true;assert(!wl_ohos_egl_wait_acquire_fence(pipefd[0]));
 assert(fcntl(importedFd,F_GETFD)==-1&&fcntl(pipefd[0],F_GETFD)>=0);
 assert(write(pipefd[1],"x",1)==1);assert(wl_ohos_egl_wait_acquire_fence(pipefd[0]));
 createFails=false;waitFails=true;assert(wl_ohos_egl_wait_acquire_fence(pipefd[0]));
 assert(currentContext==44&&finishes==oldFinishes);
 entrypoints=false;assert(wl_ohos_egl_wait_acquire_fence(pipefd[0]));
 bindFails=true;assert(wl_ohos_egl_wait_acquire_fence(pipefd[0]));bindFails=false;
 assert(fcntl(pipefd[0],F_GETFD)>=0);close(pipefd[0]);close(pipefd[1]);
 assert(!wl_ohos_egl_wait_acquire_fence(pipefd[0]));
 std::cout<<"PASS production X11 queue release: native fences, fallback, GPU acquire waits, bounded fallback, context restoration, FD ownership and unused frames\n";
}
"""
source = prelude + function('entry/src/main/cpp/display/ohos_egl_import.c',
    'static EGLImageKHR ImportNativeBufferPreserved(') + '\n' + function('entry/src/main/cpp/display/ohos_egl_import.c',
    'static int32_t CanvasBufferWaitMs(uint64_t period_ns)') + '\n' + function(
    'entry/src/main/cpp/display/ohos_egl_import.c', 'static GLenum WindowPresentGlErrors(') + '\n' + function(
    'entry/src/main/cpp/display/ohos_egl_import.c', 'bool wl_ohos_egl_wait_acquire_fence(int fd)') + '\n' + function('entry/src/main/cpp/display/ohos_egl_import.c',
    'int wl_ohos_egl_export_release_fence(void)') + '\n' + function(
    'entry/src/main/cpp/display/ohos_buffer.cpp', 'int32_t ConsumerReleaseQueueSlot(') + main
cpp=BUILD/'probe.cpp';cpp.write_text(source)
subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',str(cpp),'-o',str(BUILD/'probe')],check=True)
subprocess.run([str(BUILD/'probe')],check=True,timeout=10)
