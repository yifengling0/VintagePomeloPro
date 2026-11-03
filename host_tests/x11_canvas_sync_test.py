#!/usr/bin/env python3
"""Execute production output-slot synchronization and scene-texture lookup."""
from pathlib import Path
import subprocess
ROOT=Path(__file__).resolve().parents[1]
BUILD=ROOT/'build/host_tests/x11-canvas-sync'
BUILD.mkdir(parents=True,exist_ok=True)
def function(file,signature):
    s=(ROOT/file).read_text();start=s.index(signature);a=s.index('{',start);b=a+1;depth=1
    while depth:
        depth+=(s[b]=='{')-(s[b]=='}');b+=1
    return s[start:b]
source=r"""
#include <cassert>
#include <cstdint>
#include <fcntl.h>
#include <unistd.h>
#include <vector>
#include <iostream>
struct NativeWindow {};
using OHNativeWindow=NativeWindow;
struct OHNativeWindowBuffer {};
struct wlr_buffer {};
using GLuint=unsigned;using GLint=int;using GLenum=unsigned;
constexpr int WLR_ERROR=1,WLR_DEBUG=2;
constexpr GLenum GL_FRAMEBUFFER=1,GL_FRAMEBUFFER_BINDING=2,GL_COLOR_ATTACHMENT0=3;
constexpr GLenum GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE=4,GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME=5,GL_TEXTURE=6;
static void* g_renderer=(void*)1;
static OHNativeWindowBuffer storage;
static int rc=0,fd=-1,aborts=0,waits=0,requested=0,logs=0;
static bool wait_ok=true,no_buffer=false;
static GLuint fbo=9,binding=17;
static GLint type=GL_TEXTURE,texture=23;
static int queries=0;
int OH_NativeWindow_NativeWindowRequestBuffer(OHNativeWindow*,OHNativeWindowBuffer** out,int* fence){
 ++requested;*out=no_buffer?nullptr:&storage;*fence=fd;return rc;
}
bool wl_ohos_egl_wait_acquire_fence(int f){++waits;assert(f==fd);if(f>=0)assert(fcntl(f,F_GETFD)>=0);return wait_ok;}
int OH_NativeWindow_NativeWindowAbortBuffer(OHNativeWindow*,OHNativeWindowBuffer* b){assert(b==&storage);++aborts;return 0;}
void wlr_log(int,const char*,...){++logs;}
GLuint wlr_gles2_renderer_get_buffer_fbo(void*,wlr_buffer*){return fbo;}
void glGetIntegerv(GLenum n,GLint* v){assert(n==GL_FRAMEBUFFER_BINDING);*v=binding;}
void glBindFramebuffer(GLenum n,GLuint v){assert(n==GL_FRAMEBUFFER);binding=v;}
void glGetFramebufferAttachmentParameteriv(GLenum n,GLenum a,GLenum p,GLint* v){
 assert(n==GL_FRAMEBUFFER&&a==GL_COLOR_ATTACHMENT0&&binding==fbo);++queries;
 *v=p==GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE?type:texture;
}
"""
source+=function('entry/src/main/cpp/display/ohos_buffer.cpp','static OHNativeWindowBuffer *AcquirePresentQueueSlot(')
source+=function('entry/src/main/cpp/display/ohos_egl_import.c','static GLuint WindowSceneTexture(')
source+=r"""
int main(){
 NativeWindow window;wlr_buffer frame;
 for(int i=0;i<1000;++i){
  fd=open("/dev/null",O_RDONLY);assert(fd>=0);
  assert(AcquirePresentQueueSlot(&window)==&storage);
  assert(fcntl(fd,F_GETFD)==-1&&aborts==0);
  assert(WindowSceneTexture(&frame)==23&&binding==17);
 }
 assert(waits==1000&&queries==2000);
 fd=open("/dev/null",O_RDONLY);wait_ok=false;
 assert(!AcquirePresentQueueSlot(&window)&&aborts==1&&fcntl(fd,F_GETFD)==-1);
 rc=-1;wait_ok=true;fd=open("/dev/null",O_RDONLY);
 assert(!AcquirePresentQueueSlot(&window)&&aborts==1&&fcntl(fd,F_GETFD)==-1&&waits==1001);
 rc=0;no_buffer=true;fd=open("/dev/null",O_RDONLY);
 assert(!AcquirePresentQueueSlot(&window)&&aborts==1&&fcntl(fd,F_GETFD)==-1&&waits==1001);
 no_buffer=false;fd=-1;assert(AcquirePresentQueueSlot(&window)==&storage);
 type=0;assert(WindowSceneTexture(&frame)==0&&binding==17);
 type=GL_TEXTURE;texture=0;assert(WindowSceneTexture(&frame)==0&&binding==17);
 texture=23;fbo=0;assert(WindowSceneTexture(&frame)==0&&binding==17);
 std::cout<<"PASS 1000 output slot/scene reuse cycles: wait before write, bounded failed-wait abort, FD closure, exact texture identity and FBO restoration\n";
}
"""
# The final draw borrows the scene addon: it must not create or delete siblings.
present=function('entry/src/main/cpp/display/ohos_egl_import.c','int wl_ohos_egl_window_present(')
assert 'WindowSceneTexture(frame)' in present
assert all(n not in present for n in ['ImportNativeBufferPreserved(', 'glGenTextures(', 'glDeleteTextures(', 'destroy_image('])
assert present.index('glBindFramebuffer(GL_FRAMEBUFFER, 0)') < present.index('glDrawArrays(')
assert present.index('glDisable(GL_SCISSOR_TEST)') < present.index('glClear(')
cpp=BUILD/'probe.cpp';cpp.write_text(source)
subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',str(cpp),'-o',str(BUILD/'probe')],check=True)
subprocess.run([str(BUILD/'probe')],check=True,timeout=10)
