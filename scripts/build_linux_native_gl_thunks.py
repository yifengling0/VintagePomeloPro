#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Build matched FEX GL guest thunks in Linux/WSL, without host package installs.

Requires the verified source tree from build_linux_native_fex.py, a Linux Clang 15+,
Ubuntu 22.04 GCC 11 development headers/multilib runtime, and the existing
GNU ARM64 graphics SDK. Host and guest GL thunks are built as one closure.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tarfile
from linux_component_identity import tree_identity

ROOT = Path(__file__).resolve().parents[1]

def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--force-rebuild', action='store_true', help='Regenerate and recompile all four GL thunks')
    parser.add_argument('--source', required=True, type=Path)
    parser.add_argument('--core-receipt', required=True, type=Path)
    parser.add_argument('--tooling', type=Path, help='Existing verified LLVM15/multilib development closure')
    parser.add_argument('--tooling-cache',type=Path,default=ROOT/'build/downloads/fex-tooling')
    parser.add_argument('--output-dir', required=True, type=Path)
    parser.add_argument('--clang', required=True, type=Path)
    parser.add_argument('--gnu-cxx', required=True, type=Path)
    parser.add_argument('--dev-prefix', required=True, type=Path)
    parser.add_argument('--runtime', required=True, type=Path)
    parser.add_argument('--mesa-source',required=True,type=Path)
    args = parser.parse_args()
    if os.name != 'posix':
        parser.error('Run in Linux/WSL')
    cc = args.clang.resolve(strict=True)
    cxx = cc.with_name('clang++').resolve(strict=True)
    host_cxx=args.gnu_cxx.resolve(strict=True)
    if not subprocess.check_output([host_cxx,'-dumpmachine'],text=True,timeout=10).strip().startswith('aarch64'):
        raise ValueError('Expected GNU ARM64 host compiler')
    dev=args.dev_prefix.resolve(strict=True)
    runtime=args.runtime.resolve(strict=True)
    mesa=args.mesa_source.resolve(strict=True)
    host_sysroot=Path(subprocess.check_output([host_cxx,'-print-sysroot'],text=True,timeout=10).strip()).resolve(strict=True)
    source=args.source.resolve(strict=True)
    core=json.loads(args.core_receipt.read_text())
    if core['source']['revision'] != '37265b109a4bba9e10241972dfcb3d1fdb8f3f9a':
        raise ValueError('Pinned Valve FEX source required')
    for name,digest in core['sourceTree'].items():
        if sha(source/name)!=digest: raise ValueError('Stale FEX source: '+name)
    WORK=args.output_dir.resolve(); WORK.mkdir(parents=True,exist_ok=True)
    from prepare_linux_fex_tooling import prepare
    deps=args.tooling.resolve(strict=True) if args.tooling else prepare(WORK/'tooling',args.tooling_cache)
    llvm = deps/'usr/lib/llvm-15'
    generator = source/'ThunkLibs/Generator'
    library = source/'ThunkLibs/libGL'
    includes = source/'ThunkLibs/include'
    sources = [*generator.glob('*.cpp'), *generator.glob('*.h'),
               *library.glob('*.cpp'), *library.glob('*.h'), *includes.rglob('*.h')]
    runtimes = {'llvm': Path('/usr/lib/x86_64-linux-gnu/libLLVM-15.so.1').resolve(strict=True),
                'stdc++32': Path('/usr/lib32/libstdc++.so.6').resolve(strict=True),
                'stdc++64': Path('/usr/lib/x86_64-linux-gnu/libstdc++.so.6').resolve(strict=True)}
    epoch=int(core['inputs']['sourceDateEpoch'])
    build_env=os.environ | {'SOURCE_DATE_EPOCH':str(epoch),'TZ':'UTC','LC_ALL':'C'}
    identity = dict(sourceDateEpoch=epoch, upstreamRevision=core['source']['revision'], sourceSha256=sha(args.core_receipt),
                    patches=core['inputs']['gameContractSha256'],
                    buildHelpers={n:sha(ROOT/n) for n in ['scripts/linux_component_identity.py','scripts/prepare_linux_fex_tooling.py','docs/linux-steam/fex-gl-tooling.lock.json']},
                    headerClosure=tree_identity([('gnu',host_sysroot),('gcc',host_cxx.parent.parent/'lib/gcc'),
                      ('dev',dev/'include'),('mesa',mesa/'include'),('guest-cxx',Path('/usr/include/c++/11')),
                      ('guest-arch',Path('/usr/include/x86_64-linux-gnu')),('libc',Path('/usr/include'))]),
                    tooling={str(p.relative_to(deps)):sha(p) for p in sorted(deps.rglob('*')) if p.is_file()}, compilerSha256=sha(cxx),
                    scriptSha256=sha(Path(__file__)),
                    hostCompilerSha256=sha(host_cxx),
                    hostHeaders={str(p):sha(p) for p in (dev/'include/X11/Xlib.h',dev/'include/X11/Xproto.h',dev/'include/xcb/xcb.h',
                        mesa/'include/KHR/khrplatform.h',mesa/'include/GL/gl.h',mesa/'include/GL/glx.h')},
                    hostGlSha256=sha(runtime/'graphics/libGL.so.1'),
                    sources={p.relative_to(source).as_posix(): sha(p) for p in sorted(sources)},
                    hostLibraries={name: sha(p) for name, p in runtimes.items()})
    receipt = WORK/'build.json'
    old = json.loads(receipt.read_text()) if receipt.is_file() else {}
    if not args.force_rebuild and old.get('inputs') == identity and old.get('files') and all(
            (ROOT/row['path']).is_file() and sha(ROOT/row['path']) == row['sha256']
            for row in old['files'].values()):
        print(json.dumps(dict(reused=True, files=old['files'])))
        return
    tool = WORK/'thunkgen'
    tool_sources = [generator/(name+'.cpp') for name in ('main', 'analysis', 'data_layout', 'gen')]
    tool_inputs={k:identity[k] for k in ('compilerSha256','sourceSha256','tooling','hostLibraries')}
    tool_inputs['sources']={p.relative_to(source).as_posix():sha(p) for p in sorted(generator.glob('*')) if p.suffix in ('.cpp','.h')}
    tool_stamp=WORK/'thunkgen-build.json'
    old_tool=json.loads(tool_stamp.read_text()) if tool_stamp.exists() else {}
    if old_tool.get('inputs')!=tool_inputs or not tool.exists() or sha(tool)!=old_tool.get('sha256'):
        subprocess.run([cxx, '--driver-mode=g++', '--target=x86_64-linux-gnu', '--gcc-toolchain=/usr', '-std=c++20', '-O1', '-DNDEBUG',
                    '-I'+str(llvm/'include'), '-I'+str(source/'External/fmt/include'),
                    '-DCLANG_RESOURCE_DIR="'+str(llvm/'lib/clang/15.0.7')+'"',
                    *tool_sources, llvm/'lib/libclang-cpp.so.15', runtimes['llvm'], '-lcrypto',
                    '-DFMT_HEADER_ONLY', '-o', tool], check=True, timeout=300, env=build_env)
        tool_stamp.write_text(json.dumps(dict(inputs=tool_inputs,sha256=sha(tool)),indent=2)+'\n')
    env = build_env | {'LD_LIBRARY_PATH': str(llvm/'lib')}
    files = {}
    commands = {}
    for bits in (32, 64):
        out = WORK/('GuestThunks_32' if bits == 32 else 'GuestThunks')
        out.mkdir(exist_ok=True)
        # Use the same plain-char model in both analyses and both compilers.
        # ARM64's unsigned char model matches FEX's char/uint8_t pointer
        # repacking. Mixing x86 analysis defaults with ARM64 generated code
        # produces incompatible guest_layout<char*> return wrappers.
        flags = ['-ffile-prefix-map='+str(source)+'=/usr/src/fex', '-ffile-prefix-map='+str(WORK)+'=/usr/src/fex-thunks', '-std=c++20', '-funsigned-char', '-m'+str(bits), '--target='+('i686' if bits == 32 else 'x86_64')+'-linux-gnu',
                 '-isystem', str(includes)]
        if bits == 32:
            flags += ['-DIS_32BIT_THUNK', '-isystem', str(deps/'usr/include/x86_64-linux-gnu/c++/11/32'),
                      '-isystem', '/usr/include/x86_64-linux-gnu']
        generated = out/'thunkgen_guest_libGL.inl'
        subprocess.run([tool, library/'libGL_interface.cpp', 'libGL', '-guest', generated, '/',
                        *(['-for-32bit-guest'] if bits == 32 else []), '--', *flags],
                       env=env, check=True, timeout=120)
        x11arch = 'i386' if bits == 32 else 'x86_64'
        x11 = deps/('usr/lib/'+x11arch+'-linux-gnu/libX11.so.6.4.0')
        pending = out/'libGL.pending'
        binary = out/'libGL-guest.so'
        command = [cxx, '--driver-mode=g++', *flags, '--gcc-toolchain=/usr', '-O2', '-fPIC', '-shared', '-fwrapv',
                   '-msse2', '-mfpmath=sse', '-fno-plt', '-fno-stack-protector',
                   '-DGUEST_THUNK_LIBRARY', '-I'+str(out), library/'libGL_Guest.cpp', '-pthread',
                   x11, '-nostdlib++', runtimes['stdc++'+str(bits)], '-ldl',
                   '-Wl,-z,now,-z,relro,-z,notext,-soname,libGL.so.1', '-o', pending]
        try:
            subprocess.run(command, check=True, timeout=120, env=build_env)
            header = pending.read_bytes()[:20]
            if (header[:6] != b'\x7fELF'+bytes((1 if bits == 32 else 2, 1))
                    or int.from_bytes(header[18:20], 'little') != (3 if bits == 32 else 62)):
                raise ValueError('Wrong GL guest ELF ABI')
            pending.replace(binary)
        finally:
            pending.unlink(missing_ok=True)
        name = 'usr/share/fex-emu/'+out.name+'/libGL-guest.so'
        files[name] = dict(path=str(binary), bytes=binary.stat().st_size, sha256=sha(binary))
        commands[str(bits)] = list(map(str, command))
        host_out=WORK/('HostThunks_32' if bits==32 else 'HostThunks')
        host_out.mkdir(exist_ok=True)
        host_generated=host_out/'thunkgen_host_libGL.inl'
        # Host analysis is LP64 even for i386 guests. Passing -m32 here would
        # generate invalid host wrappers.
        host_analysis=['-std=c++20','-funsigned-char','-isystem',str(includes)]
        if bits==32:
            host_analysis+=['-DIS_32BIT_THUNK','-idirafter',str(deps/'usr/include/x86_64-linux-gnu/c++/11/32')]
        subprocess.run([tool,library/'libGL_interface.cpp','libGL','-host',host_generated,'/',
                        *(['-for-32bit-guest'] if bits==32 else []),'--',*host_analysis],env=env,check=True,timeout=120)
        host_binary=host_out/'libGL-host.so'
        command=[cxx,'--driver-mode=g++','--target=aarch64-linux-gnu','--gcc-toolchain='+str(host_cxx.parent.parent),
                 '--sysroot='+str(host_sysroot),'-ffile-prefix-map='+str(source)+'=/usr/src/fex','-ffile-prefix-map='+str(WORK)+'=/usr/src/fex-thunks','-std=c++20','-O2','-fPIC','-shared','-fwrapv','-funsigned-char',
                 '-ferror-limit=6','-DARCHITECTURE_arm64=1',
                 *(['-DIS_32BIT_THUNK'] if bits==32 else []),'-I'+str(host_out),'-I'+str(includes),
                 '-I'+str(dev/'include'),'-I'+str(mesa/'include'),library/'libGL_Host.cpp',
                 '-L'+str(runtime/'graphics'),'-L'+str(runtime),
                 '-Wl,-rpath-link,'+str(runtime/'graphics')+':'+str(runtime),
                 '-Wl,--no-undefined,-z,now,-z,relro,-soname,libGL-host.so',
                 '-l:libGL.so.1','-ldl','-pthread','-o',host_binary]
        subprocess.run(command,check=True,timeout=120,env=build_env)
        header=host_binary.read_bytes()[:20]
        if header[:6]!=b'\x7fELF\x02\x01' or int.from_bytes(header[18:20],'little')!=183:
            raise ValueError('Wrong GL host ELF ABI')
        name='usr/lib/aarch64-linux-gnu/fex-emu/'+host_out.name+'/libGL-host.so'
        files[name]=dict(path=str(host_binary),bytes=host_binary.stat().st_size,sha256=sha(host_binary))
        commands['host'+str(bits)]=list(map(str,command))
    for name,digest in identity['sources'].items():
        if sha(source/name)!=digest: raise ValueError('Thunk sources changed during build')
    report = dict(inputs=identity, files=files, commands=commands,
                  glxProcUsesGuestEntrypoints=True, glxGuestErrorAndDisplayLifetime=True, deviceValidated=False)
    receipt.write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(dict(reused=False, files=files)))


if __name__ == '__main__':
    main()
