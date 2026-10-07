"""Exercise the production fault handler against an actual protected host page.

API allocation and logging are seams. Both old and patched handler bodies come
from the pinned FEX source plus product patches; no ARM JIT is simulated here.
"""
from pathlib import Path
import os,subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
PIN='86ff33bbe2'
PATHS=['Source/Windows/Common/CallRetStack.h','Source/Windows/WOW64/Module.cpp','Source/Windows/ARM64EC/Module.cpp']

def function(source, signature):
    start=source.index(signature);end=source.index('{',start)+1;depth=1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[start:end]

STUBS=r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <sys/mman.h>
#include <unistd.h>
namespace FEXCore {
namespace Utils { constexpr uint64_t FEX_PAGE_SIZE=4096,FEX_PAGE_MASK=~(FEX_PAGE_SIZE-1); }
namespace Core { struct InternalThreadState { void* CallRetStackBase; static constexpr uint64_t CALLRET_STACK_SIZE=0x400000; }; }
}
namespace LogMan::Msg { template<class...T> void DFmt(const char*,T...) {} }
constexpr unsigned MEM_COMMIT=0x1000,PAGE_READWRITE=4;
static unsigned commits;
static bool fail_commit;
void* VirtualAlloc(void* p,size_t n,unsigned f,unsigned prot) {
  assert(f==MEM_COMMIT && prot==PAGE_READWRITE && n==4096 && !(uintptr_t(p)&4095));
  ++commits;
  if(fail_commit || mprotect(p,n,PROT_READ|PROT_WRITE))return nullptr;
  return p;
}
struct CallRetStackInfo { uint64_t AllocationBase,AllocationEnd,DefaultLocation; };
'''
MAIN=r'''
int main() {
  using namespace FEXCore;
  const auto size=Core::InternalThreadState::CALLRET_STACK_SIZE;
  auto mapping=mmap(nullptr,size+8192,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
  assert(mapping!=MAP_FAILED);
  Core::InternalThreadState thread {static_cast<char*>(mapping)+4096};
  auto base=uintptr_t(thread.CallRetStackBase),slot=base+size/4+48;
  uint64_t native_rsp=0x144f860;
  assert(OldHandleAccessViolation(&thread,slot,native_rsp));
  assert(native_rsp!=0x144f860);
  puts("old handler reproduces native guest-RSP corruption");
  native_rsp=0x144f860;
  assert(HandleAccessViolation(&thread,slot,native_rsp,false));
  assert(native_rsp==0x144f860 && commits==1);
  assert(*reinterpret_cast<volatile uint64_t*>(slot)==0);
  puts("native payload retry restores accessibility and preserves native registers");
  for(auto address:{base-1,base+size,base+size+4095,base-4096,base-4097,base+size+4096}) {
    auto before=commits;
    assert(!HandleAccessViolation(&thread,address,native_rsp,false));
    assert(native_rsp==0x144f860 && commits==before);
  }
  puts("native guard pages and unrelated ranges remain inaccessible");
  fail_commit=true;
  assert(!HandleAccessViolation(&thread,base+4096,native_rsp,false));
  assert(native_rsp==0x144f860);fail_commit=false;
  uint64_t jit_sp=0;
  assert(HandleAccessViolation(&thread,base-1,jit_sp,true));
  assert(jit_sp==base+size/4);
  jit_sp=0;
  assert(HandleAccessViolation(&thread,base+size,jit_sp,true));
  assert(jit_sp==base+size/4);
  assert(!HandleAccessViolation(&thread,base+size+4096,jit_sp,true));
  puts("JIT guard recovery and native allocation failure semantics preserved");
  munmap(mapping,size+8192);
}
'''

class NativeCallRetFault(unittest.TestCase):
    def test_build_overlay_replay(self):
        script=(ROOT/'scripts/build_fex.sh').read_text()
        overlay=script[script.index('# Keep these separate from patched_source:'):script.index('# POSIX read-only is zero.')]
        with tempfile.TemporaryDirectory() as temp:
            tree=Path(temp)
            for path in PATHS:
                p=tree/path;p.parent.mkdir(parents=True,exist_ok=True)
                p.write_text(subprocess.check_output(['git','-C',str(ROOT/'thirdparty/fex'),'show',f'{PIN}:{path}'],text=True))
            env=os.environ.copy();env.update(FEX_SRC=str(tree),SCRIPT_DIR=str(ROOT/'scripts'))
            command='set -euo pipefail\nlog() { :; }; err() { echo "$*" >&2; exit 1; };\n'+overlay
            subprocess.run(['bash','-c',command],env=env,check=True)
            after={path:(tree/path).read_bytes() for path in PATHS}
            subprocess.run(['bash','-c',command],env=env,check=True)
            self.assertEqual(after,{path:(tree/path).read_bytes() for path in PATHS})

    def test_production_handler(self):
        with tempfile.TemporaryDirectory() as temp:
            tree=Path(temp)
            for path in PATHS:
                p=tree/path;p.parent.mkdir(parents=True,exist_ok=True)
                p.write_text(subprocess.check_output(['git','-C',str(ROOT/'thirdparty/fex'),'show',f'{PIN}:{path}'],text=True))
            for patch in ['fex-wow64-synthetic-return.patch','fex-windows-native-callret-fault.patch']:
                if patch.startswith('fex-windows-native'):
                    old=(tree/PATHS[0]).read_text()
                with (ROOT/'scripts/patches'/patch).open() as f:
                    subprocess.run(['patch','--batch','-s','-p1'],cwd=tree,stdin=f,check=True)
            new=(tree/PATHS[0]).read_text()
            text=STUBS+function(new,'CallRetStackInfo GetInfoThread(')+'\n'
            text+=function(old,'bool HandleAccessViolation(').replace('HandleAccessViolation(','OldHandleAccessViolation(',1)+'\n'
            text+=function(new,'bool HandleAccessViolation(')+'\n'+MAIN
            cpp=tree/'test.cpp';cpp.write_text(text)
            subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-O2','-include','initializer_list',str(cpp),'-o',str(tree/'test')],check=True)
            result=subprocess.run([str(tree/'test')],capture_output=True,text=True,check=True)
            print(result.stdout,end='')
            wow=(tree/PATHS[1]).read_text();ec=(tree/PATHS[2]).read_text()
            self.assertIn('Context->X25, IsAddressInJit(Context->Pc)',wow)
            self.assertIn('CTX->IsAddressInCodeBuffer(Thread, NativeContext->Pc)',ec)
            self.assertIn('NativeContext->Pc < SignalDelegator->GetConfig().DispatcherEnd',ec)

if __name__=='__main__':unittest.main()
