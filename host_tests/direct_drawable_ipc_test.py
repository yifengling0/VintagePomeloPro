"""Execute production Direct IPC handlers with only platform parcel/window I/O mocked."""
from pathlib import Path
import subprocess, tempfile
R=Path(__file__).resolve().parents[1]
source=(R/'entry/src/main/cpp/proc/wine_child_ipc.cpp').read_text()
handlers=source[source.index('struct DirectSurface {'):source.index('void CloseFds(')]
acquire=source[source.index('extern "C" __attribute__((visibility("default"))) OHNativeWindow*'):source.index('extern "C" __attribute__((visibility("default"))) OHIPCRemoteStub*')]
# Exported acquire/release functions are the final functions in this unit.
stubs=r'''#include <cassert>
#include <cstdint>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <unistd.h>
#include <cstdio>
#include "proc/wine_child_ipc.h"
struct OHNativeWindow { int refs=1, destroyed=0; };
struct OHIPCParcel { winehua::wineipc::DirectSurfaceToken token{}; OHNativeWindow* window=nullptr; int index=0,result=-1,reads=0; };
#define OH_IPC_SUCCESS 0
#define OH_IPC_CHECK_PARAM_ERROR -1
#define OH_LOG_INFO(...) ((void)0)
int OH_IPCParcel_ReadInt32(const OHIPCParcel* p,int32_t* value) {
 auto* q=const_cast<OHIPCParcel*>(p);switch(q->index++) {
 case 0:*value=q->token.clientPid;break;case 1:*value=q->token.toplevelId;break;
 case 2:*value=q->token.wlSurfaceId;break;case 4:*value=q->token.width;break;
 case 5:*value=q->token.height;break;default:return -1;}return 0;
}
int OH_IPCParcel_ReadInt64(const OHIPCParcel* p,int64_t* value) {
 auto* q=const_cast<OHIPCParcel*>(p);assert(q->index++==3);*value=q->token.generation;return 0;
}
int OH_IPCParcel_WriteInt32(OHIPCParcel* p,int32_t value){p->result=value;return 0;}
int OH_NativeWindow_ReadFromParcel(OHIPCParcel* p,OHNativeWindow** value){++p->reads;*value=p->window;return 0;}
void OH_NativeWindow_DestroyNativeWindow(OHNativeWindow* w){++w->destroyed;--w->refs;}
int OH_NativeWindow_NativeObjectReference(OHNativeWindow* w){++w->refs;return 0;}
int OH_NativeWindow_NativeObjectUnreference(OHNativeWindow* w){--w->refs;return 0;}
'''
tests=r'''
int request(uint32_t op,winehua::wineipc::DirectSurfaceToken token,OHNativeWindow* window=nullptr,int* reads=nullptr){
 OHIPCParcel in{token,window},out;int result=OnSurfaceRequest(op,&in,&out);
 if(reads)*reads=in.reads;return result?result:out.result;
}
int main(){
 using namespace winehua::wineipc;
 DirectSurfaceToken parent{getpid(),5,20,1,320,240},child{getpid(),5,21,2,200,120};
 OHNativeWindow a,b,c;assert(request(kAttachSurface,parent,&a)==0);assert(request(kAttachSurface,child,&b)==0);
 uint32_t top=0;uint64_t generation=0;int32_t w=0,h=0;
 auto* first=WineHua_DirectSurfaceAcquireByWlSurface(20,&top,&generation,&w,&h);
 assert(first==&a&&top==5&&generation==1&&w==320);
 auto* second=WineHua_DirectSurfaceAcquireByWlSurface(21,&top,&generation,&w,&h);
 assert(second==&b&&top==5&&generation==2&&w==200);
 WineHua_DirectSurfaceRelease(first);WineHua_DirectSurfaceRelease(second);
 assert(a.destroyed==0&&b.destroyed==0);
 assert(WineHua_DirectSurfaceAcquire(6,21,nullptr,nullptr,nullptr)==nullptr);
 int reads=-1;assert(request(kAttachSurface,child,&b,&reads)==0&&reads==0);
 auto wrong=child;wrong.toplevelId=6;assert(request(kDetachSurface,wrong)==-1);
 auto stale=child;stale.generation=1;assert(request(kAttachSurface,stale,&c,&reads)==-1&&reads==0);
 wrong=child;wrong.clientPid++;assert(request(kAttachSurface,wrong,&c,&reads)==-1&&reads==0);
 child.width=640;assert(request(kResizeSurface,child)==0);assert(b.destroyed==0);
 assert(request(kQuerySurface,parent)==0&&request(kQuerySurface,child)==0);
 assert(request(kDetachSurface,parent)==0&&a.destroyed==1&&b.destroyed==0);
 assert(request(kQuerySurface,parent)==-1&&request(kQuerySurface,child)==0);
 parent.generation=3;assert(request(kAttachSurface,parent,&c)==0);
 stale=parent;stale.generation=1;assert(request(kDetachSurface,stale)==-1&&c.destroyed==0);
 assert(request(kDetachSurface,parent)==0&&request(kDetachSurface,child)==0);
 assert(a.refs==0&&b.refs==0&&c.refs==0);
 puts("PASS production IPC: concurrent drawables, exact owner, generation, resize and independent detach");
}
'''
with tempfile.TemporaryDirectory(prefix='direct-drawable-ipc-') as directory:
 p=Path(directory);unit=p/'test.cpp';unit.write_text(stubs+handlers+acquire+tests)
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-I',str(R/'entry/src/main/cpp'),str(unit),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True,timeout=15)
