#!/usr/bin/env python3
"""Exercise OHOS producer destructors against the pinned wlroots buffer/addon code."""
from pathlib import Path
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'build/host_tests/ohos-buffer-lifecycle'
BUILD.mkdir(parents=True, exist_ok=True)
WLR = ROOT / 'thirdparty/wlroots'
source = (ROOT / 'entry/src/main/cpp/display/ohos_buffer.cpp').read_text()

def definition(signature, struct=False):
    start = source.index(signature)
    opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end + int(struct)] + '\n'

prelude = r"""
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <vector>
#include <iostream>
extern "C" {
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/util/log.h>
}
struct OH_NativeImage { bool alive = true; };
using Native=OH_NativeImage;
using OH_NativeBuffer=Native; using OHNativeWindowBuffer=Native;
using OHNativeWindow=Native;
static std::vector<int> order;
static int addonDestroyed, signals, releases, aborts, unrefs, fences, errors;
static int acquireFd=-1,lastQueueFd=-1,references=0;
static Native* acquiredBuffer=nullptr;
static bool waitAllowed=true;
struct BufferHandle {int width=10,height=10,stride=40,format=1;};
static BufferHandle handle;
int OH_NativeImage_AcquireNativeWindowBuffer(Native*,Native** b,int* fd){
 *b=acquiredBuffer;*fd=acquireFd;return 0;
}
int OH_NativeWindow_NativeObjectReference(Native* b){assert(b->alive);++references;return 0;}
BufferHandle* OH_NativeWindow_GetBufferHandleFromNative(Native*){return &handle;}
bool wl_ohos_egl_wait_acquire_fence(int fd){assert(fd==acquireFd);return waitAllowed;}
uint32_t OhosFormatToDrm(int32_t){return 1;}
extern "C" void _wlr_log(wlr_log_importance,const char*,...){ ++errors; }
void OH_NativeWindow_DestroyNativeWindowBuffer(Native* p){
 assert(p->alive); p->alive=false; order.push_back(4);
}
void OH_NativeBuffer_Unreference(Native* p){
 assert(p->alive); p->alive=false; ++unrefs; order.push_back(5);
}
void OH_NativeWindow_NativeObjectUnreference(Native* p){
 assert(p->alive); p->alive=false; ++unrefs; order.push_back(5);
}
int OH_NativeWindow_NativeWindowAbortBuffer(Native* w,Native* b){
 assert(w->alive&&b->alive); ++aborts; order.push_back(3); return 0;
}
int OH_NativeImage_ReleaseNativeWindowBuffer(Native* w,Native* b,int fd){
 assert(w->alive&&b->alive); lastQueueFd=fd;if(fd>=0)close(fd);
 ++releases; order.push_back(3); return 0;
}
int wl_ohos_egl_export_release_fence(){++fences; order.push_back(6); return -1;}
uint64_t wl_ohos_egl_sync_count(){return 12;}
uint32_t wl_ohos_output_present_seq(){return 11;}
uint64_t g_relTotal,g_relSynced,g_relViolations,g_relUnrendered;
"""
helpers = r"""
struct Watch {
 wl_listener destroy{}, release{};
 wlr_addon addon{};
 Native* backing;
};
void onDestroy(wl_listener* listener,void*){
 auto* watch = wl_container_of(listener, (Watch*)nullptr, destroy);
 assert(watch->backing->alive); ++signals; order.push_back(1);
 wl_list_remove(&watch->destroy.link); wl_list_remove(&watch->release.link);
}
void onRelease(wl_listener*,void*){}
void onAddonDestroy(wlr_addon* addon){
 auto* watch = wl_container_of(addon, (Watch*)nullptr, addon);
 assert(watch->backing->alive); ++addonDestroyed; order.push_back(2);
 wlr_addon_finish(addon);
}
const wlr_addon_interface addonImpl={"fake-gpu-import",onAddonDestroy};
void watchBuffer(wlr_buffer* b,Watch* watch,Native* backing){
 watch->backing=backing; watch->destroy.notify=onDestroy; watch->release.notify=onRelease;
 wl_signal_add(&b->events.destroy,&watch->destroy);
 wl_signal_add(&b->events.release,&watch->release);
 wlr_addon_init(&watch->addon,&b->addons,watch,&addonImpl);
}
void retire(wlr_buffer* b, bool locked){
 const int n=addonDestroyed;
 if(locked)wlr_buffer_lock(b);
 wlr_buffer_drop(b);
 if(locked){ assert(addonDestroyed==n&&order.empty()); wlr_buffer_unlock(b); }
 assert(addonDestroyed==n+1&&signals==addonDestroyed);
 assert(order.size()>=2&&order[0]==1&&order[1]==2);
}
extern "C" wlr_buffer* wl_ohos_consumer_buffer_acquire(Native*);
int main(){
 const wlr_buffer_impl ownedImpl={BufferDestroy,nullptr,nullptr,nullptr,nullptr};
 const wlr_buffer_impl presentImpl={PresentDestroy,nullptr,nullptr,nullptr,nullptr};
 const wlr_buffer_impl consumerImpl={ConsumerDestroy,nullptr,nullptr,nullptr,nullptr};
 for(int i=0;i<3000;++i){
  // Owned buffers, output slots, and per-frame guest wrappers must all finish.
  Native backing,wrapper,window; Watch watch;
  order.clear();
  if(i%3==0){
   auto* b=static_cast<WlOhosBuffer*>(calloc(1,sizeof(WlOhosBuffer)));
   b->native=&backing; b->window_buffer=&wrapper;
   wlr_buffer_init(&b->base,&ownedImpl,10,10); watchBuffer(&b->base,&watch,&backing);
   retire(&b->base,i%2); assert((order==std::vector<int>{1,2,4,5}));
  } else if(i%3==1){
   auto* b=static_cast<WlOhosPresentBuffer*>(calloc(1,sizeof(WlOhosPresentBuffer)));
   b->window=&window; b->window_buffer=&backing; b->returned=i%2;
   bool returned=b->returned;
   wlr_buffer_init(&b->base,&presentImpl,10,10); watchBuffer(&b->base,&watch,&backing);
   retire(&b->base,i%2);
   assert(order== (returned?std::vector<int>{1,2}:std::vector<int>{1,2,3}));
  } else {
   auto* b=static_cast<WlOhosConsumerBuffer*>(calloc(1,sizeof(WlOhosConsumerBuffer)));
   b->image=&window; b->window_buffer=&backing; b->returned=i%2;
   b->handoff_valid=true; b->handoff_sync_count=11;
   bool returned=b->returned;
   wlr_buffer_init(&b->base,&consumerImpl,10,10); watchBuffer(&b->base,&watch,&backing);
   retire(&b->base,i%2);
   assert(order== (returned?std::vector<int>{1,2,5}:std::vector<int>{1,2,6,3,5}));
  }
 }
 assert(addonDestroyed==3000&&signals==3000&&unrefs==2000);
 assert(aborts==500&&releases==500&&fences==500&&g_relViolations==0);
 // Only the intentional missing-present fallback emits an error.
 assert(errors==aborts);
 Native image, backing; acquiredBuffer=&backing;
 acquireFd=open("/dev/null",O_RDONLY);waitAllowed=false;
 assert(wl_ohos_consumer_buffer_acquire(&image)==nullptr);
 assert(lastQueueFd==acquireFd&&fcntl(acquireFd,F_GETFD)==-1&&references==0);
 acquireFd=open("/dev/null",O_RDONLY);waitAllowed=true;
 auto* frame=wl_ohos_consumer_buffer_acquire(&image);
 assert(frame&&references==1&&fcntl(acquireFd,F_GETFD)==-1);
 wlr_buffer_drop(frame);assert(lastQueueFd==-1&&!backing.alive);
 std::cout<<"PASS 3000 OHOS buffers: real wlroots lock/drop, destroy events, addons before native release, queue return once and failed-acquire ownership\n";
}
"""
parts = [definition('struct ' + name + ' {', struct=True)
         for name in ('WlOhosBuffer','WlOhosPresentBuffer','WlOhosConsumerBuffer')]
parts += [definition(signature) for signature in (
    'struct WlOhosBuffer *BufferFromBase(',
    'struct WlOhosPresentBuffer *PresentFromBase(',
    'struct WlOhosConsumerBuffer *ConsumerFromBase(',
    'void PresentUnmap(', 'int32_t ConsumerReleaseQueueSlot(',
    'void BufferDestroy(', 'void PresentDestroy(', 'void ConsumerDestroy(',
    'bool ConsumerGetDmabuf(')]
parts.append(definition('const struct wlr_buffer_impl kConsumerBufferImpl =', struct=True))
cpp = BUILD / 'probe.cpp'
cpp.write_text(prelude + ''.join(parts) + definition('extern \"C\" struct wlr_buffer *wl_ohos_consumer_buffer_acquire(') + helpers)
flags = shlex.split(subprocess.check_output(
    ['pkg-config','--cflags','--libs','wayland-server >= 1.21','libdrm','pixman-1'], text=True))
common = ['-DWLR_USE_UNSTABLE','-DWLR_PRIVATE=', '-ffunction-sections', '-fdata-sections',
          '-I'+str(WLR/'include'), *flags]
objects = []
for name in ('types/buffer/buffer.c','util/addon.c'):
    obj = BUILD / (Path(name).stem + '.o')
    subprocess.run(['cc','-std=gnu11', *common, '-c',str(WLR/name),'-o',str(obj)],check=True)
    objects.append(str(obj))
subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',*common,
                str(cpp),*objects,'-Wl,--gc-sections',*flags,'-o',str(BUILD/'probe')],check=True)
subprocess.run([str(BUILD/'probe')],check=True,timeout=10)
