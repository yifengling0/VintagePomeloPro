#!/usr/bin/env python3
"""Stress real allocSlice/backing-snapshot code with concurrent vector growth."""
from pathlib import Path
import argparse,subprocess,tempfile

def function(text,marker):
    begin=text.index(marker);brace=text.index('{',begin);end=brace+1;depth=1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[begin:end]

def run(source):
    cpp=(source/'src/dxvk/dxvk_buffer.cpp').read_text();header=(source/'src/dxvk/dxvk_buffer.h').read_text()
    lookup=function(cpp,'  bool DxvkBuffer::getMappedRange(')
    alloc=function(header,'    DxvkBufferSliceHandle allocSlice()')
    push=function(header,'    void pushSlice(')
    for signature in ('  VkResult DxvkBuffer::flushMappedSlice(', '  VkResult DxvkBuffer::beginMappedSliceWrite(', '  VkResult DxvkBuffer::invalidateMappedSlice('):
        body=function(cpp,signature);assert 'getMappedRange(slice, range)' in body and 'm_buffers' not in body
    spin=(source/'src/util/sync/sync_spinlock.h').read_text();spin=spin[spin.index('namespace dxvk::sync'):]
    code=r"""
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <immintrin.h>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#define unlikely(x) (x)
#define likely(x) (x)
#define DXVK_ARCH_X86
namespace dxvk::this_thread {void yield(){std::this_thread::yield();}}
@@SPIN@@
namespace dxvk {
using VkDeviceSize=uint64_t;using VkDeviceMemory=uint64_t;
constexpr int VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE=6;
struct VkMappedMemoryRange {int sType;void* pNext;uint64_t memory,offset,size;};
uint64_t align(uint64_t x,uint64_t a){return (x+a-1)/a*a;}
struct Memory {uint64_t handle,base;explicit operator bool()const{return handle!=0;}
 uint64_t memory()const{return handle;}uint64_t offset()const{return base;}
 void* mapPtr(uint64_t x)const{return (void*)(uintptr_t(handle*4096+x));}
};
struct DxvkBufferHandle {uint64_t buffer;Memory memory;};
struct DxvkBufferSliceHandle {uint64_t handle,offset,length;void* mapPtr;};
struct Device {struct Properties {struct Core {struct Details {struct Limits {uint64_t nonCoherentAtomSize=64;} limits;} properties;} core;} props;
 const Properties& properties()const{return props;}
};
class DxvkBuffer {
public:
 Device dev;Device* m_device=&dev;
 DxvkBufferHandle m_buffer{1,{1,256}};
 mutable sync::Spinlock m_freeMutex;sync::Spinlock m_swapMutex;
 std::vector<DxvkBufferHandle> m_buffers;
 std::vector<DxvkBufferSliceHandle> m_freeSlices,m_nextSlices;
 bool m_fifoSlices=false,m_lazyAlloc=false;
 uint64_t m_physSliceCount=1,m_physSliceMaxCount=1,m_physSliceLength=33,m_physSliceStride=64;
 uint64_t next=2;
 DxvkBufferHandle allocBuffer(uint64_t count,bool clear){assert(count==1&&clear);uint64_t n=next++;return {n,{n,256}};}
 @@PUSH@@
 @@ALLOC@@
 bool getMappedRange(const DxvkBufferSliceHandle&,VkMappedMemoryRange&)const;
};
@@LOOKUP@@
}
int main(){using namespace dxvk;
 DxvkBuffer b;VkMappedMemoryRange first{};
 assert(b.getMappedRange({1,7,33,nullptr},first));
 assert(first.sType==6&&!first.pNext&&first.memory==1&&first.offset==256&&first.size==64);
 assert(!b.getMappedRange({99999999,0,1,nullptr},first));
 assert(b.m_freeMutex.try_lock());b.m_freeMutex.unlock();
 constexpr unsigned count=24000;
 auto slices=std::make_unique<DxvkBufferSliceHandle[]>(count);
 std::atomic<unsigned> ready{0};
 std::thread writer([&]{for(unsigned i=0;i<count;i++){slices[i]=b.allocSlice();ready.store(i+1,std::memory_order_release);if(i%31==0)std::this_thread::yield();}});
 std::thread reader([&]{for(unsigned i=0;i<count;i++){
  while(ready.load(std::memory_order_acquire)<=i)std::this_thread::yield();
  VkMappedMemoryRange range{};assert(b.getMappedRange(slices[i],range));
  assert(range.memory==slices[i].handle&&range.offset==256&&range.size==64);
  if(i){assert(b.getMappedRange(slices[i/2],range)&&range.memory==slices[i/2].handle);}
 }});
 writer.join();reader.join();
 assert(b.m_buffers.size()==count);
 assert(b.getMappedRange(slices[0],first)&&first.memory==2);
 assert(b.m_freeMutex.try_lock());b.m_freeMutex.unlock();
}
""".replace('@@SPIN@@',spin).replace('@@LOOKUP@@',lookup).replace('@@ALLOC@@',alloc).replace('@@PUSH@@',push)
    with tempfile.TemporaryDirectory(prefix='dxvk-backing-concurrency-') as folder:
        p=Path(folder);file=p/'test.cpp';file.write_text(code)
        base=['g++','-std=c++17','-O1','-g','-Wall','-Wextra','-Werror',str(file),'-pthread']
        subprocess.run(base+['-fsanitize=address,undefined','-o',str(p/'asan')],check=True)
        subprocess.run([str(p/'asan')],check=True)
        subprocess.run(base+['-fsanitize=thread','-fno-pie','-no-pie','-o',str(p/'tsan')],check=True)
        def tsan_run(binary):
            # Some WSL ASLR layouts prevent TSan startup. Retry only that
            # runtime failure; never hide a race report or assertion.
            for _ in range(8):
                result=subprocess.run([str(binary)],capture_output=True,text=True,timeout=30)
                if 'unexpected memory mapping' not in result.stderr:
                    return result
            return result
        result=tsan_run(p/'tsan')
        if result.returncode and 'unexpected memory mapping' in result.stderr:
            tsan='TSan runtime unavailable (unexpected memory mapping)'
        else:
            assert result.returncode==0,result.stderr
            mutant=code.replace('std::unique_lock<sync::Spinlock> freeLock(m_freeMutex);\n    const DxvkBufferHandle* backing', 'const DxvkBufferHandle* backing')
            assert mutant!=code
            file.write_text(mutant)
            subprocess.run(base+['-fsanitize=thread','-fno-pie','-no-pie','-o',str(p/'mutant')],check=True)
            failed=tsan_run(p/'mutant')
            if 'unexpected memory mapping' in failed.stderr:
                tsan='TSan PASS; mutation TSan runtime unavailable'
            else:
                assert failed.returncode!=0 and 'ThreadSanitizer: data race' in failed.stderr,failed.stderr
                tsan='TSan PASS; unlocked lookup mutation rejected with a data race'
    print('PASS: production allocSlice/getMappedRange/pushSlice and Spinlock; 24000 concurrent backing allocations with current/old-slice snapshots, unknown handle, atom alignment, released lock; ASAN/UBSAN; '+tsan)

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--source',type=Path,default=Path(__file__).resolve().parents[1]/'thirdparty/dxvk');run(parser.parse_args().source)
