#!/usr/bin/env python3
"""Exercise migrated Wine boundary functions with controlled host API results."""
import argparse
import copy
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest
from fex_wow64_synthetic_return_test import function

p=argparse.ArgumentParser();p.add_argument('--source',type=Path,required=True)
a,extra=p.parse_known_args();SOURCE=a.source.resolve()
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'scripts'))


def execute(code):
    with tempfile.TemporaryDirectory(prefix='wine-upstream-contract-') as directory:
        root=Path(directory);(root/'test.cpp').write_text(code)
        result=subprocess.run(['c++','-std=c++20','-O2','-Wall','-Wextra',str(root/'test.cpp'),'-o',str(root/'test')],capture_output=True,text=True)
        if result.returncode:raise AssertionError(result.stderr[-4000:])
        subprocess.run([str(root/'test')],check=True,timeout=10)


class MigratedWineTests(unittest.TestCase):
    def test_fresh_index_replays_exact_history_without_touching_candidate(self):
        from prepare_wine_upstream import replay_history
        lock=json.loads((ROOT/'docs/upstream-sync/wine-candidate.lock.json').read_text())
        def status():
            return subprocess.check_output(['git','-C',str(SOURCE),'status','--porcelain'])
        before=status()
        with tempfile.TemporaryDirectory(prefix='wine-history-replay-') as directory:
            repo=Path(directory)
            subprocess.run(['git','init','--bare','-q',directory],check=True)
            common=subprocess.check_output(['git','-C',str(SOURCE),'rev-parse',
                '--path-format=absolute','--git-common-dir']).decode().strip()
            (repo/'objects/info/alternates').write_text(common+'/objects\n')
            self.assertEqual(replay_history(repo,lock,repo),lock['adaptedCommit'])
            self.assertFalse((repo/'wine-replay.index').exists())
            broken=copy.deepcopy(lock)
            broken['patches'][0]['sha256']='0'*64
            with self.assertRaisesRegex(ValueError,'patch changed'):
                replay_history(repo,broken,repo)
            self.assertFalse((repo/'wine-replay.index').exists())
            broken=copy.deepcopy(lock)
            broken['adaptedTree']='0'*40
            with self.assertRaisesRegex(ValueError,'locked candidate'):
                replay_history(repo,broken,repo)
        self.assertEqual(status(),before)

    def test_self_context_identity_flush_and_error_propagation(self):
        code=r'''
#include <cassert>
#include <cstddef>
#include <cstdint>
#define WINAPI
using UINT=unsigned;using HANDLE=int;using NTSTATUS=int;
struct WOW64_CONTEXT { unsigned value; } Saved;
struct Info { unsigned CpuFlags; } InfoValue{1};
static Info *wow64info=&InfoValue;
#define WOW64_CPUFLAGS_SOFTWARE 1
static unsigned BackendCalls,NormalCalls;
static int BackendResult=200;
static HANDLE get_handle(UINT **p) { return static_cast<int>(*(*p)++); }
static WOW64_CONTEXT *get_ptr(UINT **) { return &Saved; }
static HANDLE GetCurrentThread() { return -1; }
static HANDLE GetCurrentProcess() { return 42; }
static NTSTATUS NtCompareObjects(HANDLE a,HANDLE b) { assert(b==-1);return a==-1||a==2 ? 0 : -5; }
static NTSTATUS GetContext(HANDLE h,HANDLE p,void *r,void *c) {
 assert((h==-1||h==2)&&p==42&&!r&&c==&Saved);++BackendCalls;return BackendResult;
}
static auto pBTCpuGetContext=GetContext;
static NTSTATUS RtlWow64GetThreadContext(HANDLE h,WOW64_CONTEXT *c) {
 assert(c==&Saved);++NormalCalls;return h==4 ? -5 : 100;
}
'''
        text=(SOURCE/'dlls/wow64/syscall.c').read_text()
        code+=function(text,'NTSTATUS WINAPI wow64_NtGetContextThread(')
        code+=r'''
static int call(HANDLE handle) { UINT args[]={static_cast<UINT>(handle)};return wow64_NtGetContextThread(args); }
int main() {
 assert(call(-1)==200&&BackendCalls==1&&NormalCalls==0);
 assert(call(2)==200&&BackendCalls==2&&NormalCalls==0);
 assert(call(3)==100&&BackendCalls==2&&NormalCalls==1);
 assert(call(4)==-5&&BackendCalls==2&&NormalCalls==2);
 BackendResult=-7;assert(call(2)==-7&&BackendCalls==3&&NormalCalls==2);
 InfoValue.CpuFlags=0;assert(call(-1)==100&&BackendCalls==3&&NormalCalls==3);
}
'''
        execute(code)

    def test_unix_bridge_export_selection_and_missing_table(self):
        code=r'''
#include <cassert>
#include <cstring>
using BOOL=bool;using NTSTATUS=int;
static void *Handle=(void*)0x1234;
static bool Missing;
static unsigned Lookup;
#define STATUS_SUCCESS 0
#define STATUS_ENTRYPOINT_NOT_FOUND (-17)
static void *dlsym(void *h,const char *name) {
 assert(h==Handle);++Lookup;
 assert(!strcmp(name,"__wine_unix_call_funcs")||!strcmp(name,"__wine_unix_call_wow64_funcs"));
 if(Missing)return nullptr;
 return !strcmp(name,"__wine_unix_call_funcs")?(void*)0x1000:(void*)0x1001;
}
'''
        text=(SOURCE/'dlls/ntdll/unix/virtual.c').read_text()
        code+=function(text,'static NTSTATUS get_unixlib_funcs(')
        code+=r'''
int main() {
 const void *table=nullptr;
 assert(get_unixlib_funcs(Handle,false,&table)==STATUS_SUCCESS&&table==(void*)0x1000);
 assert(get_unixlib_funcs(Handle,true,&table)==STATUS_SUCCESS&&table==(void*)0x1001);
 Missing=true;assert(get_unixlib_funcs(Handle,false,&table)==STATUS_ENTRYPOINT_NOT_FOUND&&!table);
 assert(Lookup==3);
}
'''
        execute(code)

    def test_wine_private_query_abi_keeps_fex_numbering(self):
        text=(SOURCE/'include/winternl.h').read_text()
        enum=re.search(r'typedef enum _MEMORY_INFORMATION_CLASS\s*\{.*?\} MEMORY_INFORMATION_CLASS;',text,re.S)
        self.assertIsNotNone(enum)
        execute('#define __WINESRC__\n'+enum[0]+'''\nstatic_assert(MemoryWineLoadUnixLib==1000);
static_assert(MemoryWineLoadUnixLibWow64==1001);
int main() {}\n''')


if __name__=='__main__':unittest.main(argv=[sys.argv[0],*extra],verbosity=2)
