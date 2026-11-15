#!/usr/bin/env python3
"""Execute the production UnlockBuffer and publication helper against a delayed CS.

Check captured physical slices, final-unlock ordering, dirty ranges, default-off
behavior and error propagation. Device draw tests cover the backing-copy path.
"""
from pathlib import Path
import argparse
import os
import subprocess
import tempfile

def block(text, signature):
    start=text.index(signature)
    at=text.index('{', start)
    depth=1
    end=at+1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}')
        end+=1
    return text[start:end]

def run(source):
    text=(source/'src/d3d9/d3d9_device.cpp').read_text()
    helpers=block(text,'  static bool D3D9ShadowBufferUploadEnabled()')+'\n'+block(text,'  static void PublishD3D9BufferUpload(')
    unlock=block(text,'  HRESULT D3D9DeviceEx::UnlockBuffer(')
    cpp=r'''
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
using HRESULT=int; constexpr int D3D_OK=0,D3DERR_INVALIDCALL=-1,VK_SUCCESS=0;
constexpr bool FALSE=false;
using D3D9DeviceLock=int;
enum {D3D9_COMMON_BUFFER_MAP_MODE_BUFFER,D3D9_COMMON_BUFFER_MAP_MODE_DIRECT};
enum {D3D9_COMMON_BUFFER_TYPE_MAPPING};
enum {D3DPOOL_DEFAULT,D3DPOOL_MANAGED};
template<class T> using Rc=std::shared_ptr<T>;
struct DxvkError:std::runtime_error{using std::runtime_error::runtime_error;};
namespace env {std::string getEnvVar(const char* n){auto v=std::getenv(n);return v?v:"";}}
struct DxvkBufferSliceHandle {uintptr_t handle;uint64_t offset,length;void* mapPtr;};
struct Range {uint32_t min=0,max=0;bool IsDegenerate(){return min==max;}void Clear(){min=max=0;}};
struct DxvkBuffer {int id=1;};
struct BufferDesc {int Pool=D3DPOOL_DEFAULT;};
struct D3D9CommonBuffer {
 int locks=1,mode=D3D9_COMMON_BUFFER_MAP_MODE_DIRECT,flags=99;
 Range dirty{16,48};BufferDesc desc;char memory[256]={};
 DxvkBufferSliceHandle slice{17,512,256,memory};Rc<DxvkBuffer> buffer=std::make_shared<DxvkBuffer>();
 int DecrementLockCount(){if(locks)--locks;return locks;}
 int GetMapMode(){return mode;}Range& DirtyRange(){return dirty;}
 DxvkBufferSliceHandle GetMappedSlice(){return slice;}
 template<int> Rc<DxvkBuffer> GetBuffer(){return buffer;}
 void SetMapFlags(int f){flags=f;}BufferDesc* Desc(){return &desc;}
};
struct DxvkContext {
 struct Call {Rc<DxvkBuffer> buffer;DxvkBufferSliceHandle slice;};
 std::vector<Call> calls;bool fail=false;
 int flushMappedBuffer(const Rc<DxvkBuffer>& b,const DxvkBufferSliceHandle& s){
  calls.push_back({b,s});return fail?-2:0;
 }
};
struct D3D9DeviceEx {
 std::vector<std::function<void(DxvkContext*)>> queue;int implicit=0,backed=0;
 D3D9DeviceLock LockDevice(){return 0;}
 template<class F>void EmitCs(F&& f){queue.emplace_back(std::forward<F>(f));}
 void FlushImplicit(bool){++implicit;}void FlushBuffer(D3D9CommonBuffer*){++backed;}
 HRESULT UnlockBuffer(D3D9CommonBuffer*);
 void drain(DxvkContext& c){for(auto& f:queue)f(&c);queue.clear();}
};
HELPERS
UNLOCK
int main(){
 bool enabled=std::getenv("WINEHUA_DXVK9_SHADOW_BUFFERS")[0]=='1';
 D3D9DeviceEx dev;DxvkContext ctx;
 // Nested maps must wait for the last unlock; CS publication remains deferred.
 D3D9CommonBuffer a;a.locks=2;auto saved=a.slice;auto original=a.buffer;
 assert(dev.UnlockBuffer(&a)==D3D_OK&&dev.queue.empty()&&a.dirty.min==16);
 assert(dev.UnlockBuffer(&a)==D3D_OK&&ctx.calls.empty());
 if(enabled){
  assert(dev.queue.size()==1&&a.dirty.IsDegenerate()&&a.flags==0);
  // A later DISCARD/rename must not redirect the already queued publication.
  a.slice={91,4096,128,a.memory+64};a.buffer=std::make_shared<DxvkBuffer>();
  dev.drain(ctx);assert(ctx.calls.size()==1);
  auto& c=ctx.calls[0];assert(c.buffer==original&&c.slice.handle==saved.handle);
  assert(c.slice.offset==528&&c.slice.length==32&&c.slice.mapPtr==a.memory+16);
 }else{assert(dev.queue.empty()&&a.dirty.max==48&&a.flags==99);}
 // Read-only locks do not mark a dirty range and must not publish.
 D3D9CommonBuffer ro;ro.dirty.Clear();auto before=ctx.calls.size();
 assert(dev.UnlockBuffer(&ro)==D3D_OK&&dev.queue.empty()&&ctx.calls.size()==before);
 // A nonzero partial range, representing NOOVERWRITE, stays bounded.
 D3D9CommonBuffer part;part.dirty={128,192};
 assert(dev.UnlockBuffer(&part)==D3D_OK);
 if(enabled){dev.drain(ctx);auto& c=ctx.calls.back();assert(c.slice.offset==640&&c.slice.length==64);}
 // A normal backed default-pool buffer keeps the existing upload path.
 D3D9CommonBuffer backed;backed.mode=D3D9_COMMON_BUFFER_MAP_MODE_BUFFER;
 assert(dev.UnlockBuffer(&backed)==D3D_OK&&dev.implicit==1&&dev.backed==1&&dev.queue.empty());
 D3D9CommonBuffer managed;managed.mode=D3D9_COMMON_BUFFER_MAP_MODE_BUFFER;managed.desc.Pool=D3DPOOL_MANAGED;
 assert(dev.UnlockBuffer(&managed)==D3D_OK&&dev.backed==1&&dev.implicit==1);
 if(enabled){
  // Reject invalid publication bounds before queueing any memory operation.
  D3D9CommonBuffer bad;bad.dirty={200,300};
  assert(dev.UnlockBuffer(&bad)==D3DERR_INVALIDCALL&&dev.queue.empty());
  // Bridge errors must be visible instead of silently submitting stale data.
  D3D9CommonBuffer fail;ctx.fail=true;assert(dev.UnlockBuffer(&fail)==D3D_OK);
  bool thrown=false;try{dev.drain(ctx);}catch(const DxvkError&){thrown=true;}assert(thrown);
 }
 return 0;
}
'''.replace('HELPERS',helpers).replace('UNLOCK',unlock)
    with tempfile.TemporaryDirectory(prefix='dxvk-buffer-publish-') as folder:
        p=Path(folder);(p/'test.cpp').write_text(cpp)
        subprocess.run(['g++','-std=c++17','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-variable',
                        '-fsanitize=address,undefined',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
        for mode in ('0','1'):
            subprocess.run([str(p/'test')],env={**os.environ,'WINEHUA_DXVK9_SHADOW_BUFFERS':mode},check=True)
    print('PASS: default-off, nested unlock, delayed CS, physical rename, partial range, read-only, backing route, bounds and flush error; ASAN/UBSAN')

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source',type=Path,default=Path(__file__).resolve().parents[1]/'thirdparty/dxvk')
    run(parser.parse_args().source)
