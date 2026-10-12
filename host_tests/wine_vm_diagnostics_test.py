#!/usr/bin/env python3
"""Run the actual opt-in log helpers; ensure failures cannot publish OldProtect."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='wine-vm-diagnostics-') as folder:
    tmp = Path(folder)
    source_path = tmp / 'dlls/ntdll/unix/virtual.c'
    source_path.parent.mkdir(parents=True)
    source_path.write_bytes((ROOT / 'thirdparty/wine-valve/dlls/ntdll/unix/virtual.c').read_bytes())
    patch_path = ROOT / 'patches/wine/0060-ntdll-vm-protect-diagnostics.patch'
    command = ['patch', '-p1', '--batch', '--force', '--fuzz=0', '-d', str(tmp), '-i', str(patch_path)]
    applied = subprocess.run(command + ['-R', '--dry-run'], capture_output=True).returncode == 0
    if not applied:
        subprocess.run(command, check=True, capture_output=True)
    source = source_path.read_text()
    first = source.index('static unsigned int ohos_vm_protect_begin(')
    last = source.index('/***********************************************************************\n *           virtual_relocate_module', first)
    helpers = source[first:last]
    stub = r'''
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <stdint.h>
typedef void *HANDLE;
typedef size_t SIZE_T;
typedef uint32_t ULONG, NTSTATUS;
typedef int BOOL;
#define STATUS_SUCCESS 0
'''
    main = r'''
int main(int argc, char **argv) {
    unsigned int id;
    int limit = argc > 1 ? atoi(argv[1]) : 1;
    for (int i = 0; i < limit; ++i) {
        errno = EINVAL;
        id = ohos_vm_protect_begin((HANDLE)-1, (void*)0x1000, 0x2000, 4);
        if (errno != EINVAL) return 1;
        ohos_vm_protect_end(id, argc > 2 ? 0xc000000d : 0, (void*)0x1000,
                            0x2000, 0xfeed, 1, 0x20, 0x20);
        if (errno != EINVAL) return 2;
    }
    return 0;
}
'''
    (tmp / 'test.c').write_text(stub + helpers + main)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra', '-D__OHOS__',
                    str(tmp / 'test.c'), '-o', str(tmp / 'test')], check=True)
    env = dict(os.environ)
    env.pop('WINEHUA_VM_DIAGNOSTICS', None)
    result = subprocess.run([str(tmp / 'test')], env=env, capture_output=True, text=True, check=True)
    assert not result.stderr, result.stderr
    print('PASS VM diagnostics default off')
    env['WINEHUA_VM_DIAGNOSTICS'] = '1'
    result = subprocess.run([str(tmp / 'test')], env=env, capture_output=True, text=True, check=True)
    assert result.stderr.count(' id=1 ') == 2 and 'old_valid=1 old=feed' in result.stderr, result.stderr
    print('PASS successful protection output paired with its input')
    result = subprocess.run([str(tmp / 'test'), '1', 'failure'], env=env, capture_output=True, text=True, check=True)
    assert 'status=c000000d output_valid=0' in result.stderr and 'old_valid=0 old=0' in result.stderr
    assert 'feed' not in result.stderr
    print('PASS failed protection never publishes stale OldProtect')
    result = subprocess.run([str(tmp / 'test'), '4100'], env=env, capture_output=True, text=True, check=True)
    assert result.stderr.count('[VM-PROTECT-BEGIN]') == 4096
    assert result.stderr.count('[VM-PROTECT-END]') == 4096
    assert 'id=4097' not in result.stderr
    print('PASS bounded paired diagnostics preserve errno')
