#!/usr/bin/env python3
"""Reproduce SDK symbol preemption and exercise the real app-owned dispatcher.

Uses real shared objects and the host dynamic linker, with mocked OS/fork
implementations. This verifies routing and return values, not phone execution.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
HEADER = r'''
#pragma once
#include <stdint.h>
enum Ability_NativeChildProcess_ErrCode {
    NCP_NO_ERROR=0, NCP_ERR_INTERNAL=16000050,
    NCP_ERR_MULTI_PROCESS_DISABLED=16010004, NCP_ERR_LIB_LOADING_FAILED=16010007
};
struct NativeChildProcess_Fd;
struct NativeChildProcess_Args { char* entryParams; struct { NativeChildProcess_Fd* head; } fdList; };
struct NativeChildProcess_Options { int isolationMode; };
using OH_Ability_OnNativeChildProcessStarted = void (*)(int, void*);
extern "C" Ability_NativeChildProcess_ErrCode OH_Ability_StartNativeChildProcess(
    const char*, NativeChildProcess_Args, NativeChildProcess_Options, int32_t*);
extern "C" int OH_Ability_CreateNativeChildProcess(const char*, OH_Ability_OnNativeChildProcessStarted);
'''
SYSTEM = r'''
#include <AbilityKit/native_child_process.h>
static int calls;
extern "C" int SystemCalls() { return calls; }
extern "C" Ability_NativeChildProcess_ErrCode OH_Ability_StartNativeChildProcess(
    const char*, NativeChildProcess_Args, NativeChildProcess_Options, int32_t*) {
    ++calls; return NCP_ERR_MULTI_PROCESS_DISABLED;
}
extern "C" int OH_Ability_CreateNativeChildProcess(const char*, OH_Ability_OnNativeChildProcessStarted) {
    ++calls; return NCP_ERR_MULTI_PROCESS_DISABLED;
}
'''
APP = r'''
#include "proc/ncp_dispatch.h"
#include "phone_adapter/phone_process.h"
#include <cassert>
#include <cstring>
extern "C" void PhoneAdapter_SetPhoneMode(bool);
static int callbacks;
static void Callback(int status, void* proxy) { assert(!status && proxy); ++callbacks; }
Ability_NativeChildProcess_ErrCode Phone_StartNativeChildProcess(
    const char* entry, NativeChildProcess_Args args, NativeChildProcess_Options options, int32_t* pid) {
    assert(!strcmp(args.entryParams, "payload") && options.isolationMode == 17);
    if (!strcmp(entry, "bad")) return NCP_ERR_LIB_LOADING_FAILED;
    assert(!strcmp(entry, "libwine_child.so:Main")); *pid=4242; return NCP_NO_ERROR;
}
int Phone_CreateNativeChildProcess(const char* name, OH_Ability_OnNativeChildProcessStarted callback) {
    assert(!strcmp(name, "libvirgl_child.so")); callback(0, &callbacks); return NCP_NO_ERROR;
}
extern "C" void SetPhone(bool value) { PhoneAdapter_SetPhoneMode(value); }
extern "C" int Start(bool fail) {
    NativeChildProcess_Args args{const_cast<char*>("payload"), {nullptr}};
    NativeChildProcess_Options options{17}; int32_t pid=-1;
    int status=WineHua_StartNativeChildProcess(fail ? "bad" : "libwine_child.so:Main", args, options, &pid);
    if (!status) assert(pid==4242);
    return status;
}
extern "C" int Create() {
    callbacks=0;
    int status=WineHua_CreateNativeChildProcess("libvirgl_child.so", Callback);
    if (!status) assert(callbacks==1);
    return status;
}
'''
RUNNER = r'''
#include <dlfcn.h>
#include <cassert>
#include <cstring>
#include <cstdio>
int main(int argc, char** argv) {
    assert(argc==4); void* system=nullptr; void* app=nullptr;
    const bool missing=!strcmp(argv[3], "missing"), legacy=!strcmp(argv[3], "legacy");
    const bool first=legacy || !strcmp(argv[3], "system-first");
    auto loadSystem=[&] { system=dlopen(argv[1], RTLD_NOW|RTLD_GLOBAL); assert(system); };
    auto loadApp=[&] { app=dlopen(argv[2], RTLD_NOW|RTLD_LOCAL); assert(app); };
    if (!missing && first) loadSystem(); loadApp(); if (!missing && !first) loadSystem();
    auto set=reinterpret_cast<void(*)(bool)>(dlsym(app,"SetPhone"));
    auto start=reinterpret_cast<int(*)(bool)>(dlsym(app,"Start"));
    auto create=reinterpret_cast<int(*)()>(dlsym(app,"Create"));
    auto calls=system ? reinterpret_cast<int(*)()>(dlsym(system,"SystemCalls")) : nullptr;
    assert(set && start && create);
    set(true);
    if (legacy) {
        assert(start(false)==16010004 && create()==16010004 && calls()==2);
        puts("reproduced: system-first loading bypasses the legacy phone wrappers"); return 0;
    }
    assert(!start(false) && !create());
    assert(start(true)==16010007); // Preserve a genuine phone loader failure.
    if (calls) assert(!calls());
    set(false); const int expected=missing ? 16000050 : 16010004;
    assert(start(false)==expected && create()==expected);
    if (calls) assert(calls()==2);
    set(true); assert(!start(false) && !create());
    if (calls) assert(calls()==2);
    printf("explicit dispatch PASS (%s; phone and tablet return values preserved)\n", argv[3]);
}
'''


def main():
    with tempfile.TemporaryDirectory(prefix='ncp-linkage-') as directory:
        task = Path(directory)
        (task / 'AbilityKit').mkdir()
        (task / 'AbilityKit/native_child_process.h').write_text(HEADER)
        flags = ['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                 '-I' + str(task), '-I' + str(ROOT / 'entry/src/main/cpp')]
        for name, body in [('system.cpp', SYSTEM), ('app.cpp', APP), ('runner.cpp', RUNNER)]:
            (task / name).write_text(body)
        system = task / 'libchild_process.so'
        subprocess.run([*flags, '-shared', '-fPIC', str(task / 'system.cpp'),
                        '-Wl,-soname,libchild_process.so', '-o', str(system)], check=True)
        dispatch = ROOT / 'entry/src/main/cpp/proc/ncp_dispatch.cpp'
        library = task / 'libentry.so'
        subprocess.run([*flags, '-shared', '-fPIC', str(dispatch), str(task / 'app.cpp'),
                        '-ldl', '-o', str(library)], check=True)
        dynamic = subprocess.check_output(['readelf', '--dyn-syms', '-W', str(library)], text=True)
        relocations = subprocess.check_output(['readelf', '-rW', str(library)], text=True)
        assert 'OH_Ability_StartNativeChildProcess' not in dynamic
        assert 'OH_Ability_CreateNativeChildProcess' not in dynamic
        assert 'WineHua_StartNativeChildProcess' not in dynamic + relocations
        assert 'WineHua_CreateNativeChildProcess' not in dynamic + relocations
        runner = task / 'runner'
        subprocess.run([*flags, '-Wno-misleading-indentation', str(task / 'runner.cpp'),
                        '-ldl', '-o', str(runner)], check=True)
        for order in ['system-first', 'app-first', 'missing']:
            subprocess.run([str(runner), str(system), str(library), order], check=True, timeout=10)
        # Reproduce the exact old design using the same process and load order.
        legacy = task / 'legacy.cpp'
        legacy.write_text(dispatch.read_text().replace('WineHua_StartNativeChildProcess(',
                          'OH_Ability_StartNativeChildProcess(').replace(
                          'WineHua_CreateNativeChildProcess(', 'OH_Ability_CreateNativeChildProcess('))
        legacy_app = task / 'legacy_app.cpp'
        legacy_app.write_text(APP.replace('WineHua_StartNativeChildProcess(',
                              'OH_Ability_StartNativeChildProcess(').replace(
                              'WineHua_CreateNativeChildProcess(', 'OH_Ability_CreateNativeChildProcess('))
        old = task / 'legacy-entry.so'
        subprocess.run([*flags, '-shared', '-fPIC', str(legacy), str(legacy_app),
                        '-I' + str(dispatch.parent), '-ldl', '-o', str(old)], check=True)
        subprocess.run([str(runner), str(system), str(old), 'legacy'], check=True, timeout=10)


if __name__ == '__main__':
    main()
