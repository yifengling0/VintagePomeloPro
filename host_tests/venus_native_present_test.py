#!/usr/bin/env python3
"""Execute production NativePresentLocked/EndFrame with failed driver operations."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
def method(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]
present = (ROOT / "entry/src/main/cpp/graphics/venus_surface_presenter.cpp").read_text()
target = (ROOT / "entry/src/main/cpp/graphics/native_window_vk_target.cpp").read_text()
code = r"""
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
using VkResult=int; using VkImage=uintptr_t; using VkImageLayout=int;
using VkSemaphore=uintptr_t; using VkFence=uintptr_t; using VkPipelineStageFlags=int;
constexpr int VK_SUCCESS=0, VK_ERROR_DEVICE_LOST=-4, VK_ERROR_INITIALIZATION_FAILED=-3;
constexpr int VK_STRUCTURE_TYPE_SUBMIT_INFO=4, VK_PIPELINE_STAGE_TRANSFER_BIT=1;
constexpr int VK_IMAGE_LAYOUT_UNDEFINED=0, VK_IMAGE_LAYOUT_GENERAL=1;
struct VkSubmitInfo {int sType; unsigned waitSemaphoreCount=0; const VkSemaphore* pWaitSemaphores=nullptr;
 const int* pWaitDstStageMask=nullptr; unsigned commandBufferCount=0; const uintptr_t* pCommandBuffers=nullptr;
 unsigned signalSemaphoreCount=0; const VkSemaphore* pSignalSemaphores=nullptr;};
enum class NativeWindowVkBeginResult {Ready, Deferred, Failed};
const char* NativeWindowVkBeginResultName(NativeWindowVkBeginResult) {return "begin-failed";}
#define OH_LOG_WARN(...) ((void)0)
#define OH_LOG_INFO(...) ((void)0)
#define LOG_APP 0
std::string events; int fail=0, aborted=0, closed=0, destroyed=0, abandoned=0; bool pending=false;
uint64_t clockNs=100000000;
uint64_t NowNs(){return clockNs+=1000;}
uint64_t NowUs(){return NowNs()/1000;}
void CloseFd(int* fd){if(*fd>=0)++closed; *fd=-1;}
struct Region {void* rects; int rectNumber;};
constexpr int SET_UI_TIMESTAMP=1;
int OH_NativeWindow_NativeWindowHandleOpt(int,int,uint64_t){return 0;}
int OH_NativeWindow_NativeWindowFlushBuffer(int,int,int,Region){events+='F';return fail==7?-1:0;}
void OH_NativeWindow_NativeWindowAbortBuffer(int,int){assert(!pending);++aborted;}
int vkResetFences(int,unsigned,const uintptr_t*){events+='R';return fail==4?-1:0;}
int vkQueueSubmit(int,unsigned,const VkSubmitInfo* s,uintptr_t){events+='Q';assert(s->waitSemaphoreCount==1&&s->signalSemaphoreCount==1); if(fail==5)return -1;pending=true;return 0;}
struct NativeWindowVkTarget {
 struct Slot{int windowBuffer=1;}; Slot slot; Slot* current_=&slot; int window_=1;
 uint64_t lastFlushUs_=0;
 bool SetRequestTimeoutMs(int){return fail!=1;}
 NativeWindowVkBeginResult BeginFrame(){return fail==2?NativeWindowVkBeginResult::Failed:fail==8?NativeWindowVkBeginResult::Deferred:NativeWindowVkBeginResult::Ready;}
 int AcquireGpu(uintptr_t){events+='A';return fail==3?-1:0;}
 uintptr_t ColorImage(){return 10;} int ColorFormat(){return 37;}
 int SignalRelease(int,unsigned,const uintptr_t*,int* fd){events+='S';if(fail==6)return -1;*fd=77;return 0;}
 size_t ImportedSlotCount(){return 1;}
 void Abandon(){++abandoned;current_=nullptr;}
 // END_FRAME
};
struct Harness {
 struct Frame{uintptr_t command=1,acquired=2,released=3,complete=4;bool submitted=false;};
 NativeWindowVkTarget vkDirect_; bool nativeFailed_=false; int device_=1,queue_=2;
 unsigned sourceWidth_=800,sourceHeight_=600,lastSerial_=0;
 uint64_t displayPeriodNs_=16666667,lastPresentNs_=0,framesPresented_=0,serialRegressions_=0;
 uint64_t throttled_=0,totalPresentUs_=0,maxPresentUs_=0,firstPresentedNs_=0;
 bool deviceReleasing_=true,surfaceAttached_=true;uint64_t surfaceKey_=123;
 bool MatchesDeviceLocked(uint32_t context,uintptr_t device){return context==1&&device==1;}
 void DestroyVulkanLocked(){assert(!pending);++destroyed;}
 void ClearVulkanStateLocked(){events+='X';}
 void ReleaseWindowLocked(){events+='W';}
 std::mutex mutex_;
 int RecordPresentCopyLocked(Frame&,uintptr_t,int,uintptr_t,int,unsigned,unsigned,int,int,bool){events+='C';return fail==9?-1:0;}
 // PRESENT
 // RELEASE
};
void unlock(void*){events+='U';}
void reset(int which){fail=which;events.clear();pending=false;aborted=closed=destroyed=abandoned=0;}
int main(){
 {reset(0);Harness h;Harness::Frame f;uint64_t deadline=0;
  assert(h.NativePresentLocked(f,99,1,1,&deadline,unlock,nullptr)==0);
  assert(events=="ACRQSUF"&&f.submitted&&pending&&!h.nativeFailed_);
  assert(h.vkDirect_.current_==nullptr&&aborted==0&&h.framesPresented_==1);}
 for(int which=1;which<=9;++which){
  reset(which);Harness h;Harness::Frame f;uint64_t deadline=0;
  int result=h.NativePresentLocked(f,99,1,1,&deadline,unlock,nullptr);
  if(which==8){assert(result==1&&deadline&&h.throttled_==1&&!h.nativeFailed_);continue;}
  assert(result==-EAGAIN&&h.nativeFailed_&&h.framesPresented_==0);
  assert(aborted==0); // No buffer returned while a failed submission may still own it.
  if(which==6||which==7){assert(pending&&f.submitted&&h.vkDirect_.current_);assert(events.find('U')!=std::string::npos);}
  if(which==9){assert(events=="AC"&&!pending);} // recording failure does not reset the fence
  if(which==7){assert(closed==1);pending=false;OH_NativeWindow_NativeWindowAbortBuffer(1,1);assert(aborted==1);}
 }
 {reset(0);Harness h;assert(h.FinishDeviceRelease(1,1,VK_SUCCESS));assert(destroyed==1&&abandoned==0&&!h.deviceReleasing_);}
 for(int result : {2,VK_ERROR_DEVICE_LOST,VK_ERROR_INITIALIZATION_FAILED}) {
  reset(0);Harness h;pending=true;h.surfaceAttached_=false;
  assert(h.FinishDeviceRelease(1,1,result));
  assert(destroyed==0&&abandoned==1&&aborted==0&&!h.deviceReleasing_&&h.surfaceKey_==0);
 }
 {reset(0);Harness h;assert(!h.FinishDeviceRelease(2,1,VK_SUCCESS));assert(destroyed==0&&abandoned==0&&h.deviceReleasing_);}
 puts("Production Venus native present: asynchronous success, faults, deferred ownership and device retirement PASS");
}
"""
end=method(target,"int32_t NativeWindowVkTarget::EndFrame(")
end=end.replace("NativeWindowVkTarget::EndFrame", "EndFrame")
code=code.replace("// END_FRAME",end)
code=code.replace("// PRESENT",method(present,"    int NativePresentLocked("))
code=code.replace("// RELEASE",method(present,"    bool FinishDeviceRelease("))
code=code.replace("#include <cstdio>","#include <cstdio>\n#include <mutex>")
with tempfile.TemporaryDirectory(prefix="vp-venus-native-") as tmp:
    path=Path(tmp)/"test.cpp"
    path.write_text(code)
    binary=Path(tmp)/"test"
    subprocess.run(["c++","-std=c++17","-O2","-Wall","-Wextra",str(path),"-o",str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
