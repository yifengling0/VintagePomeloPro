#!/usr/bin/env python3
"""Compile the actual FEX tracker with mocked OS/Context boundaries.

The interval implementation and tracker logic are not reimplemented here.
--baseline proves the pinned implementation fails the same behavioral tests.
"""
from pathlib import Path
import argparse, os, subprocess, tempfile

parser=argparse.ArgumentParser()
parser.add_argument('--baseline',action='store_true')
parser.add_argument('--source',type=Path)
args=parser.parse_args()
root=Path(__file__).resolve().parents[1]
fex=args.source or root/'thirdparty/fex'

headers={
'windef.h':r'''
#pragma once
#include <cstdint>
#include <cstddef>
#include <limits>
#include <algorithm>
using DWORD=uint32_t; using ULONG=uint32_t; using SIZE_T=size_t;
using NTSTATUS=int32_t; using LPCVOID=const void*; using HMODULE=void*;
constexpr ULONG PAGE_NOACCESS=1, PAGE_READONLY=2, PAGE_READWRITE=4,
 PAGE_WRITECOPY=8, PAGE_EXECUTE=16, PAGE_EXECUTE_READ=32,
 PAGE_EXECUTE_READWRITE=64, PAGE_EXECUTE_WRITECOPY=128, PAGE_GUARD=256;
constexpr ULONG MEM_COMMIT=0x1000, IMAGE_SCN_MEM_EXECUTE=0x20000000, IMAGE_SCN_MEM_WRITE=0x80000000;
struct MEMORY_BASIC_INFORMATION {void* BaseAddress; void* AllocationBase; SIZE_T RegionSize; ULONG State; ULONG Protect;};
struct IMAGE_SECTION_HEADER {uint32_t Characteristics; uint32_t VirtualAddress; union {uint32_t VirtualSize;} Misc;};
struct IMAGE_NT_HEADERS {struct {uint16_t NumberOfSections;} FileHeader; IMAGE_SECTION_HEADER Sections[1];};
#define IMAGE_FIRST_SECTION(nt) ((nt)->Sections)
''',
'winternl.h':r'''
#pragma once
#include "windef.h"
constexpr int MemoryBasicInformation=0;
inline void* NtCurrentProcess(){return reinterpret_cast<void*>(-1);}
SIZE_T VirtualQuery(LPCVOID,MEMORY_BASIC_INFORMATION*,SIZE_T);
NTSTATUS NtQueryVirtualMemory(void*,void*,int,MEMORY_BASIC_INFORMATION*,SIZE_T,void*);
NTSTATUS NtProtectVirtualMemory(void*,void**,SIZE_T*,ULONG,ULONG*);
inline IMAGE_NT_HEADERS* RtlImageNtHeader(HMODULE p){return static_cast<IMAGE_NT_HEADERS*>(p);}
''',
'FEXCore/fextl/vector.h':r'''
#pragma once
#include <vector>
namespace fextl {template<class T> using vector=std::vector<T>;}
''',
'FEXCore/Utils/LogManager.h':r'''
#pragma once
namespace LogMan::Msg {
template<class... T> void DFmt(const char*,T... ){}
template<class... T> void IFmt(const char*,T... ){}
template<class... T> void EFmt(const char*,T... ){}
}
''',
'FEXCore/Utils/TypeDefines.h':r'''
#pragma once
#include <cstdint>
namespace FEXCore::Utils {constexpr uint64_t FEX_PAGE_SIZE=4096,FEX_PAGE_MASK=~uint64_t(4095);}
''',
'FEXCore/Utils/SignalScopeGuards.h':'#pragma once\n',
'FEXCore/Debug/InternalThreadState.h':r'''
#pragma once
namespace FEXCore::Core {struct InternalThreadState {};}
''',
'FEXCore/HLE/SyscallHandler.h':r'''
#pragma once
#include "windef.h"
namespace FEXCore::HLE {struct ExecutableRangeInfo {uint64_t Base=0,Size=0; bool Writable=false; bool NeedsCodeValidation=false;};}
''',
'FEXCore/Config/Config.h':r'''
#pragma once
#include <string_view>
namespace FEXCore::Config {enum {CONFIG_SMC_NONE,CONFIG_SMC_MTRACK,CONFIG_SMC_FULL};}
struct ConfigValue {
 std::string_view key;
 int operator()() const {return key=="SMCCHECKS" ? 1 : key=="MAXINST" ? 1000 : 0;}
 operator int() const{return operator()();}
};
#define FEX_CONFIG_OPT(name,key) ConfigValue name{#key}
''',
'FEXCore/Core/Context.h':r'''
#pragma once
#include <mutex>
#include <vector>
#include <utility>
#include "windef.h"
#include <FEXCore/Debug/InternalThreadState.h>
namespace FEXCore::Context {
class Context {
 std::mutex lock;
public:
 std::vector<std::pair<uint64_t,uint64_t>> invalidations;
 std::mutex& GetCodeInvalidationMutex(){return lock;}
 void MarkMonoDetected(){}
 void MarkMonoBackpatcherBlock(uint64_t){}
 bool IsAddressInCodeBuffer(FEXCore::Core::InternalThreadState*,uint64_t){return true;}
 uint64_t RestoreRIPFromHostPC(FEXCore::Core::InternalThreadState*,uint64_t){return 0;}
 uint64_t GetGuestBlockEntry(FEXCore::Core::InternalThreadState*){return 0;}
 void InvalidateCodeBuffersCodeRange(uint64_t a,uint64_t n){invalidations.emplace_back(a,n);}
 void InvalidateThreadCachedCodeRange(FEXCore::Core::InternalThreadState*,uint64_t,uint64_t){}
};
}
''',
}

test=r'''
#include <cstdio>
#include <stdexcept>
#include <map>
#include <functional>
#include <FEXCore/Core/Context.h>
#include <FEXCore/Utils/TypeDefines.h>
#include <shared_mutex>
#include <unordered_map>
#include <string_view>
#include <FEXCore/Utils/IntervalList.h>
// Expose the existing private disable operation only to this test harness.
#define private public
#include "InvalidationTracker.h"
#undef private
#include <winternl.h>
static std::map<uint64_t,ULONG> pages;
static std::map<uint64_t,uint64_t> allocations;
static bool failProtect=false;
static unsigned protectCalls=0;
SIZE_T VirtualQuery(LPCVOID,MEMORY_BASIC_INFORMATION*,SIZE_T){return 0;}
NTSTATUS NtQueryVirtualMemory(void*,void* a,int,MEMORY_BASIC_INFORMATION* info,SIZE_T,void*) {
 auto base=uint64_t(a)&~uint64_t(4095); auto it=pages.find(base);
 if(it==pages.end())return -1;
 auto allocation=allocations.count(base)?allocations[base]:base;
 *info={reinterpret_cast<void*>(base),reinterpret_cast<void*>(allocation),4096,MEM_COMMIT,it->second};return 0;
}
NTSTATUS NtProtectVirtualMemory(void*,void** a,SIZE_T* size,ULONG prot,ULONG* old) {
 ++protectCalls; if(failProtect)return -1;
 auto base=uint64_t(*a)&~uint64_t(4095); auto end=(uint64_t(*a)+*size+4095)&~uint64_t(4095);
 for(auto p=base;p<end;p+=4096)if(!pages.count(p))return -1;
 *old=pages[base];for(auto p=base;p<end;p+=4096)pages[p]=prot;
 *a=reinterpret_cast<void*>(base);*size=end-base;return 0;
}
static void check(bool good,const char* message){if(!good)throw std::runtime_error(message);}
struct Fixture {
 FEXCore::Context::Context ctx;
 std::unordered_map<DWORD,FEXCore::Core::InternalThreadState*> threads;
 FEX::Windows::InvalidationTracker tracker{ctx,threads};
 FEXCore::Core::InternalThreadState thread;
 void guest(uint64_t a,uint64_t n,ULONG prot){
   for(auto p=a;p<a+n;p+=4096)pages[p]=prot;
   tracker.HandleMemoryProtectionNotification(a,n,prot);
 }
 void arm(uint64_t a,uint64_t n){
#ifdef TEST_BASELINE
   tracker.ReprotectRWXIntervals(a,n);
#else
   // Exercise the retained tracker fallback independently of the public
   // writable-code path, which now uses byte validation without RX arming.
   tracker.ProtectRWXIntervalsInternal(a,n,false);
#endif
 }
 bool fault(uint64_t a,uint64_t type=1){
#ifdef TEST_BASELINE
   return tracker.HandleRWXAccessViolation(&thread,1,a);
#else
   return tracker.HandleRWXAccessViolation(&thread,1,a,type);
#endif
 }
};
static void run(const char* name,const std::function<void()>& fn,int& failures){
 pages.clear();allocations.clear();failProtect=false;protectCalls=0;
 try{fn();std::printf("PASS %s\n",name);}catch(const std::exception& e){++failures;std::printf("FAIL %s: %s\n",name,e.what());}
}
int main(){int failures=0;
#ifndef TEST_BASELINE
 run("WOW64 process DEP controls legacy readable execution without changing pages",[]{
   Fixture f;f.guest(0x1000,4096,PAGE_READWRITE);f.guest(0x2000,4096,PAGE_READONLY);
   check(!f.tracker.QueryExecutableRange(0x1000).Size,"DEP-on RW became executable");
   f.tracker.HandleProcessExecuteFlagsChange(2);
   auto rw=f.tracker.QueryExecutableRange(0x1000),ro=f.tracker.QueryExecutableRange(0x2000);
   check(rw.Size&&rw.Writable&&rw.NeedsCodeValidation,"legacy RW code lacks byte validation");
   check(ro.Size&&!ro.Writable,"legacy RO permission incorrect");
   check(pages[0x1000]==PAGE_READWRITE&&pages[0x2000]==PAGE_READONLY&&protectCalls==0,"DEP changed page protections");
   auto count=f.ctx.invalidations.size();f.tracker.HandleProcessExecuteFlagsChange(2);
   check(f.ctx.invalidations.size()==count,"unchanged policy invalidated again");
   f.tracker.HandleProcessExecuteFlagsChange(1);
   check(!f.tracker.QueryExecutableRange(0x1000).Size,"DEP-on retained implicit executable range");
   check(f.ctx.invalidations.size()>count,"policy transition did not invalidate cached code");
 },failures);
 run("DEP-off cannot admit guard noaccess or addresses beyond WOW64",[]{
   Fixture f;f.guest(0x1000,4096,PAGE_READWRITE|PAGE_GUARD);f.guest(0x2000,4096,PAGE_NOACCESS);
   f.guest(0x100000000ULL,4096,PAGE_READWRITE);f.tracker.HandleProcessExecuteFlagsChange(2);
   check(!f.tracker.QueryExecutableRange(0x1000).Size,"guard became implicitly executable");
   check(!f.tracker.QueryExecutableRange(0x2000).Size,"noaccess became executable");
   check(!f.tracker.QueryExecutableRange(0x100000000ULL).Size,"WOW64 policy affected native high mappings");
 },failures);
 run("legacy executable writable boundaries protection and remap are tracked",[]{
   Fixture f;f.tracker.HandleProcessExecuteFlagsChange(2);
   f.guest(0x1000,4096,PAGE_READONLY);f.guest(0x2000,4096,PAGE_READWRITE);f.guest(0x3000,4096,PAGE_READONLY);
   for(uint64_t a=0x1000;a<0x4000;a+=0x100){
     auto r=f.tracker.QueryExecutableRange(a);
     for(auto p=r.Base;p<r.Base+r.Size;p+=0x100)
       check(r.Writable==(p>=0x2000&&p<0x3000),"legacy decode range spans unlike write permissions");
   }
   auto count=f.ctx.invalidations.size();f.guest(0x2000,4096,PAGE_NOACCESS);
   check(!f.tracker.QueryExecutableRange(0x2000).Size&&f.ctx.invalidations.size()>count,"legacy protection change retained cached code");
   f.tracker.InvalidateAlignedInterval(0x1000,4096,true);
   check(!f.tracker.QueryExecutableRange(0x1000).Size,"released legacy code still executable");
   f.guest(0x1000,4096,PAGE_READWRITE);
   check(f.tracker.QueryExecutableRange(0x1000).NeedsCodeValidation,"remapped legacy writable code not validated");
 },failures);
 run("writable code keeps visible permissions and uses local validation",[]{
   Fixture f;f.guest(0x1000,4096,PAGE_EXECUTE_READWRITE);
   f.tracker.ReprotectRWXIntervals(0x1000,4096);
   auto r=f.tracker.QueryExecutableRange(0x1000);
   check(r.Writable&&r.NeedsCodeValidation,"writable code not validated");
   check(pages[0x1000]==PAGE_EXECUTE_READWRITE&&protectCalls==0,"public path hid guest WRITE");
   check(!f.fault(0x1000),"byte-validation path owns arbitrary fault");
 },failures);
 run("validation survives RX restore and clears on release",[]{
   Fixture f;f.guest(0x1000,4096,PAGE_EXECUTE_READWRITE);
   f.guest(0x1000,4096,PAGE_EXECUTE_READ);
   auto r=f.tracker.QueryExecutableRange(0x1000);
   check(!r.Writable&&r.NeedsCodeValidation,"RX restore lost validation or gained write");
   f.tracker.InvalidateAlignedInterval(0x1000,4096,true);
   f.guest(0x1000,4096,PAGE_EXECUTE_READ);
   check(!f.tracker.QueryExecutableRange(0x1000).NeedsCodeValidation,"new mapping inherited validation");
 },failures);
 run("RX -> RW -> RX tracks legitimate code-publishing transition",[]{
   Fixture f;f.guest(0x1000,4096,PAGE_EXECUTE_READ);
   f.guest(0x1000,4096,PAGE_READWRITE);
   f.guest(0x1000,4096,PAGE_EXECUTE_READ);
   auto r=f.tracker.QueryExecutableRange(0x1000);
   check(!r.Writable&&r.NeedsCodeValidation,"code-publishing transition not validated");
 },failures);
 run("query splits validation history independently from current write state",[]{
   Fixture f;f.guest(0x1000,0x4000,PAGE_EXECUTE_READ);
   f.guest(0x2000,4096,PAGE_EXECUTE_READWRITE);f.guest(0x2000,4096,PAGE_EXECUTE_READ);
   for(uint64_t a=0x1000;a<0x5000;a+=0x100){
     auto r=f.tracker.QueryExecutableRange(a);
     for(auto p=r.Base;p<r.Base+r.Size;p+=0x100)
       check(r.NeedsCodeValidation==(p>=0x2000&&p<0x3000),"range spans unlike validation modes");
   }
 },failures);
#endif
 run("RWX -> RX/R/NOACCESS preserves real write faults",[]{
   for(auto prot:{PAGE_EXECUTE_READ,PAGE_READONLY,PAGE_NOACCESS}){
     Fixture f;f.guest(0x1000,0x1000,PAGE_EXECUTE_READWRITE);f.arm(0x1000,4096);
     f.guest(0x1000,0x1000,prot);
     check(!f.tracker.QueryExecutableRange(0x1000).Writable,"stale writable state");
     check(!f.fault(0x1000),"real RX/R/noaccess fault swallowed");check(pages[0x1000]==prot,"guest protection changed");
   }
 },failures);
 run("tracker internal RX retains legal guest RWX",[]{
   Fixture f;f.guest(0x1000,4096,PAGE_EXECUTE_READWRITE);f.arm(0x1000,4096);
   check(pages[0x1000]==PAGE_EXECUTE_READ,"tracking not armed");
   check(f.tracker.QueryExecutableRange(0x1000).Writable,"internal arm erased logical write");
   check(f.fault(0x1007),"legal tracked write not claimed");check(pages[0x1000]==PAGE_EXECUTE_READWRITE,"write not restored");
 },failures);
 run("homogeneous range before/inside/after RWX island",[]{
   Fixture f;f.guest(0x1000,0x4000,PAGE_EXECUTE_READ);f.guest(0x2000,4096,PAGE_EXECUTE_READWRITE);
   for(uint64_t a=0x1000;a<0x5000;a+=0x100){
     auto r=f.tracker.QueryExecutableRange(a);check(r.Base<=a&&r.Base+r.Size>a,"query not enclosed");
     for(auto p=r.Base;p<r.Base+r.Size;p+=0x100)check(r.Writable==(p>=0x2000&&p<0x3000),"returned range spans unlike permissions");
   }
 },failures);
 run("partial permission change leaves adjacent RWX",[]{
   Fixture f;f.guest(0x1000,0x3000,PAGE_EXECUTE_READWRITE);f.guest(0x2000,4096,PAGE_EXECUTE_READ);
   check(f.tracker.QueryExecutableRange(0x1000).Writable,"left range lost");
   check(!f.tracker.QueryExecutableRange(0x2000).Writable,"middle still writable");
   check(f.tracker.QueryExecutableRange(0x3000).Writable,"right range lost");
 },failures);
 run("read/execute AVs are not SMC writes",[]{
   for(uint64_t type:{0ULL,8ULL,2ULL}){
     Fixture f;f.guest(0x1000,4096,PAGE_EXECUTE_READWRITE);f.arm(0x1000,4096);
     auto n=protectCalls;check(!f.fault(0x1000,type),"non-write fault claimed");check(protectCalls==n,"non-write fault changed protection");
   }
 },failures);
 run("failed unprotect is not reported handled",[]{
   Fixture f;f.guest(0x1000,4096,PAGE_EXECUTE_READWRITE);f.arm(0x1000,4096);failProtect=true;
   check(!f.fault(0x1000),"failed NtProtect reported success");check(pages[0x1000]==PAGE_EXECUTE_READ,"failed host protect changed page");
   failProtect=false;check(f.fault(0x1000),"failed unprotect lost tracking ownership");
 },failures);
 run("unarmed writable range does not own arbitrary AV",[]{
   Fixture f;f.guest(0x1000,4096,PAGE_EXECUTE_READWRITE);check(!f.fault(0x1000),"unarmed fault claimed");
 },failures);
 run("guard page is never tracker-unprotected",[]{
   Fixture f;f.guest(0x1000,4096,PAGE_EXECUTE_READWRITE|PAGE_GUARD);f.arm(0x1000,4096);
   check(!f.fault(0x1000),"guest guard fault swallowed");check(pages[0x1000]==(PAGE_EXECUTE_READWRITE|PAGE_GUARD),"guard removed");
 },failures);
 run("COW restoration preserves write-copy",[]{
   Fixture f;f.guest(0x1000,4096,PAGE_EXECUTE_WRITECOPY);f.arm(0x1000,4096);
   check(f.fault(0x1000),"COW tracked write not claimed");check(pages[0x1000]==PAGE_EXECUTE_WRITECOPY,"COW turned into plain RWX");
 },failures);
 run("unmap/remap clears writable and tracker state",[]{
   Fixture f;f.guest(0x1000,4096,PAGE_EXECUTE_READWRITE);f.arm(0x1000,4096);
   f.tracker.InvalidateAlignedInterval(0x1000,4096,true);pages.erase(0x1000);f.guest(0x1000,4096,PAGE_EXECUTE_READ);
   check(!f.fault(0x1000),"new mapping inherited tracking");check(!f.tracker.QueryExecutableRange(0x1000).Writable,"new mapping inherited write");
 },failures);
 run("failed arm does not establish tracking ownership",[]{
   Fixture f;f.guest(0x1000,4096,PAGE_EXECUTE_READWRITE);failProtect=true;f.arm(0x1000,4096);failProtect=false;
   check(!f.fault(0x1000),"failed arm was treated as armed");
 },failures);

 run("mixed RWX/COW disable restores each protection",[]{
   Fixture f;f.guest(0x1000,0x3000,PAGE_EXECUTE_READWRITE);f.guest(0x2000,4096,PAGE_EXECUTE_WRITECOPY);
   f.arm(0x1000,0x3000);f.tracker.DisableSMCDetection();
   check(pages[0x1000]==PAGE_EXECUTE_READWRITE,"first RWX not restored");
   check(pages[0x2000]==PAGE_EXECUTE_WRITECOPY,"COW island overwritten during disable");
   check(pages[0x3000]==PAGE_EXECUTE_READWRITE,"last RWX not restored");
 },failures);
 run("failed disable keeps ownership of armed pages",[]{
   Fixture f;f.guest(0x1000,4096,PAGE_EXECUTE_READWRITE);f.arm(0x1000,4096);
   failProtect=true;f.tracker.DisableSMCDetection();failProtect=false;
   check(f.fault(0x1000),"failed disable orphaned its RX tracking page");
   check(pages[0x1000]==PAGE_EXECUTE_READWRITE,"remaining tracking page not restored");
 },failures);
 run("mapped image follows effective page protection and COW",[]{
   alignas(4096) static uint8_t image[12288] {};
   const auto base=reinterpret_cast<uint64_t>(image);
   auto* nt=reinterpret_cast<IMAGE_NT_HEADERS*>(image);nt->FileHeader.NumberOfSections=1;
   nt->Sections[0].Characteristics=IMAGE_SCN_MEM_EXECUTE|IMAGE_SCN_MEM_WRITE;
   nt->Sections[0].VirtualAddress=4096;nt->Sections[0].Misc.VirtualSize=1;
   pages[base]=PAGE_READONLY;pages[base+4096]=PAGE_EXECUTE_WRITECOPY;
   pages[base+8192]=PAGE_EXECUTE_READWRITE|PAGE_GUARD;
   for(auto p=base;p<base+sizeof(image);p+=4096)allocations[p]=base;
   Fixture f;f.tracker.HandleImageMap("synthetic.dll",base);
   check(f.tracker.QueryExecutableRange(base+4096+100).Writable,"executable page tail absent from tracker");
   check(!f.tracker.QueryExecutableRange(base+8192).Writable,"mapped guard incorrectly armed");
   f.arm(base+4096,4096);
   check(f.fault(base+4096+100),"mapped COW write not tracked");
   check(pages[base+4096]==PAGE_EXECUTE_WRITECOPY,"mapped COW restored as RWX");
 },failures);
 run("empty/overflow notification cannot change existing ranges",[]{
   Fixture f;f.guest(0x1000,4096,PAGE_EXECUTE_READWRITE);
   f.tracker.HandleMemoryProtectionNotification(0x1007,0,PAGE_NOACCESS);
   check(f.tracker.QueryExecutableRange(0x1000).Writable,"empty notification revoked valid mapping");
   f.tracker.HandleMemoryProtectionNotification(0x1000,UINT64_MAX,PAGE_NOACCESS);
   check(f.tracker.QueryExecutableRange(0x1000).Writable,"overflow notification revoked valid mapping");
 },failures);
 run("changed effective protection cannot be tracker-unprotected",[]{
   for(auto prot:{PAGE_NOACCESS,PAGE_EXECUTE_READ|PAGE_GUARD}){
     Fixture f;f.guest(0x1000,4096,PAGE_EXECUTE_READWRITE);f.arm(0x1000,4096);
     pages[0x1000]=prot;
     check(!f.fault(0x1000),"non-tracker protection fault claimed");check(pages[0x1000]==prot,"external protection overwritten");
   }
 },failures);
 std::printf("%d failures\n",failures);return failures ? 1 : 0;
}
'''

with tempfile.TemporaryDirectory(prefix='fex-vm-contract-') as folder:
    tmp=Path(folder);src=tmp/'source';stubs=tmp/'stubs'
    paths=['Source/Windows/Common/InvalidationTracker.cpp','Source/Windows/Common/InvalidationTracker.h',
        'Source/Windows/WOW64/Module.cpp','Source/Windows/ARM64EC/Module.cpp']
    for rel in paths:
        p=src/rel;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes((fex/rel).read_bytes())
    if not args.baseline and not args.source:
        # Replay the complete overlay while compiling only the actual tracker.
        import re
        overlay=(root/'scripts/patches/fex-windows-vm-contract.patch').read_text()
        for rel in re.findall(r'^--- a/(.+)$',overlay,re.M):
            p=src/rel
            if not p.exists():
                p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes((fex/rel).read_bytes())
        with (root/'scripts/patches/fex-windows-vm-contract.patch').open('rb') as patch:
            subprocess.run(['patch','-s','-p1','-d',str(src)],stdin=patch,check=True)
    for rel,content in headers.items():
        p=stubs/rel;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(content)
    (tmp/'test.cpp').write_text(test)
    command=[os.environ.get('CXX','g++'),'-std=c++20','-pthread','-Wall','-Wextra','-fpermissive',
        '-I'+str(stubs),'-I'+str(src/'Source/Windows/Common'),'-I'+str(fex/'FEXCore/include'),
        str(tmp/'test.cpp'),str(src/'Source/Windows/Common/InvalidationTracker.cpp'),'-o',str(tmp/'test')]
    if args.baseline: command.append('-DTEST_BASELINE=1')
    subprocess.run(command,check=True)
    raise SystemExit(subprocess.run([str(tmp/'test')],timeout=30).returncode)
