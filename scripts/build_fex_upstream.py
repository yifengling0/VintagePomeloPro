#!/usr/bin/env python3
"""Build the locked candidate pair plus its matching OHOS Unix bridge.

Production pins and build_fex.sh remain untouched until device acceptance.
The optional Docker runner uses the existing workspace mount and caches.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import struct
import subprocess
import time

from prepare_fex_upstream import LOCK, ROOT, prepare
from fex_simd_runtime import pe_kind


def sha(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def pe_sections(data):
    offset = struct.unpack_from('<I', data, 60)[0]
    count = struct.unpack_from('<H', data, offset + 6)[0]
    symtab, symbols = struct.unpack_from('<II', data, offset + 12)
    strings = symtab + symbols * 18
    start = offset + 24 + struct.unpack_from('<H', data, offset + 20)[0]
    sections = []
    for i in range(count):
        name, vsize, address, size, raw, _, _, _, _, flags = struct.unpack_from('<8sIIIIIIHHI', data, start + i * 40)
        name = name.rstrip(b'\0')
        if name.startswith(b'/'):
            index = strings + int(name[1:])
            name = data[index:data.index(0, index)]
        if flags & 0xe0000000 and not name.startswith((b'.debug', b'.zdebug', b'.gnu_debug')):
            # llvm-strip fills executable file-alignment padding with INT3.
            # Only VirtualSize bytes belong to the section's mapped contents.
            sections.append((name, vsize, address, flags, data[raw:raw+min(size, vsize)]))
    return sections


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--cache', type=Path, required=True)
    parser.add_argument('--reuse', type=Path)
    parser.add_argument('--llvm-mingw', type=Path, required=True)
    parser.add_argument('--ohos-sdk', type=Path, default=Path('/apps/harmony/sdk/default/openharmony'))
    parser.add_argument('--jobs', type=int, default=8)
    parser.add_argument('--container')
    parser.add_argument('--container-root', default='/data/src/winehua')
    args = parser.parse_args()
    source, build = args.source.resolve(), args.build.resolve()
    prepare(source, args.cache.resolve(), args.reuse.resolve() if args.reuse else None, True)
    lock = json.loads(LOCK.read_text())
    build.mkdir(parents=True, exist_ok=True)
    def mapped(path):
        path = Path(path)
        return Path(args.container_root) / path.relative_to(ROOT) if args.container and path.is_relative_to(ROOT) else path
    src, output, mingw = map(mapped, (source, build, args.llvm_mingw.resolve()))
    sdk = args.ohos_sdk
    q = lambda value: shlex.quote(str(value))
    commands = ['set -euo pipefail', 'export PATH=' + q(mingw / 'bin') + ':$PATH']
    alternates = source / '.git/objects/info/alternates'
    if alternates.exists():
        paths = [str(mapped(Path(path))) for path in alternates.read_text().splitlines()]
        commands.append('export GIT_ALTERNATE_OBJECT_DIRECTORIES=' + q(':'.join(paths)))
    for name, triple, target, flags in (
            ('ec', 'arm64ec-w64-mingw32', 'arm64ecfex', ''),
            ('pe', 'aarch64-w64-mingw32', 'wow64fex', '-DFEX_WOW64_EXACT_STORE=1')):
        cmd = ['cmake', '-S', str(src), '-B', str(output / ('fex-' + name)),
               '-DCMAKE_BUILD_TYPE=RelWithDebInfo', '-DCMAKE_TOOLCHAIN_FILE=' + str(src / 'Data/CMake/toolchain_mingw.cmake'),
               '-DCMAKE_CXX_FLAGS=' + flags, '-DENABLE_LTO=False', '-DBUILD_TESTING=False',
               '-DMINGW_TRIPLE=' + triple, '-DOVERRIDE_HASH=' + lock['adaptedCommit'],
               '-DOVERRIDE_VERSION=ohos-upstream-' + lock['adaptedCommit'][:10]]
        commands += [shlex.join(cmd), shlex.join(['cmake', '--build', str(output / ('fex-' + name)),
                                                '--parallel', str(args.jobs), '--target', target])]
    commands += ['mkdir -p ' + q(output / 'staged'),
                 shlex.join([str(sdk / 'native/llvm/bin/clang++'), '--target=aarch64-linux-ohos',
                             '--sysroot=' + str(sdk / 'native/sysroot'), '-std=c++20', '-O2', '-DNDEBUG',
                             '-fPIC', '-shared', '-Wl,--no-undefined',
                             str(src / 'Source/Windows/UnixLib/FEXUnixLib.cpp'),
                             '-o', str(output / 'staged/libarm64ecfex.so')])]
    commands.append(shlex.join(['cp', str(output / 'staged/libarm64ecfex.so'), str(output / 'staged/libwow64fex.so')]))
    for name, filename in (('ec', 'libarm64ecfex.dll'), ('pe', 'libwow64fex.dll')):
        commands.append(shlex.join([str(mingw / 'bin/llvm-strip'), '--strip-all',
                                    str(output / ('fex-' + name) / 'Bin' / filename),
                                    '-o', str(output / 'staged' / filename)]))
    script = '\n'.join(commands) + '\n'
    command = ['docker', 'exec', '-i', args.container, 'bash', '-s'] if args.container else ['bash', '-s']
    started = time.monotonic()
    with (build / 'compile.log').open('wb') as log:
        process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        process.stdin.write(script.encode())
        process.stdin.close()
        written, tail = 0, b''
        for chunk in iter(lambda: process.stdout.read(65536), b''):
            room = max(0, 32 * 1024 * 1024 - written)
            log.write(chunk[:room])
            written += min(room, len(chunk))
            tail = (tail + chunk)[-65536:]
        result = process.wait()
    if result:
        raise RuntimeError('Candidate build failed: ' + tail.decode(errors='replace')[-3500:])
    modules = {}
    for name, filename, expected in (('ec', 'libarm64ecfex.dll', 'arm64ec'), ('pe', 'libwow64fex.dll', 'arm64')):
        original = build / ('fex-' + name) / 'Bin' / filename
        staged = build / 'staged' / filename
        if staged.read_bytes()[64:80] != b'Wine builtin DLL':
            raise ValueError('FEX Unix bridge requires the Wine builtin PE marker: ' + filename)
        if pe_kind(staged.read_bytes()) != expected or pe_sections(original.read_bytes()) != pe_sections(staged.read_bytes()):
            raise ValueError('Strip changed executable sections or architecture: ' + filename)
        cache = (build / ('fex-' + name) / 'CMakeCache.txt').read_text()
        if 'CMAKE_BUILD_TYPE:STRING=RelWithDebInfo' not in cache or '-DNDEBUG' not in cache:
            raise ValueError('Candidate must retain production optimization and NDEBUG')
        flags = next(line for line in cache.splitlines() if line.startswith('CMAKE_CXX_FLAGS:STRING='))
        if ('FEX_WOW64_EXACT_STORE' in flags) != (name == 'pe') or 'STRICT_MUL24' in flags or 'DIAGNOSTICS' in flags:
            raise ValueError('Unexpected experimental compile flags')
        modules[filename] = {'sha256': sha(staged), 'bytes': staged.stat().st_size,
                             'architecture': expected, 'buildSha256': sha(original)}
    bridge = build / 'staged/libarm64ecfex.so'
    data = bridge.read_bytes()
    if data[:6] != b'\x7fELF\x02\x01' or struct.unpack_from('<H', data, 18)[0] != 183:
        raise ValueError('Unix bridge must be OHOS AArch64 ELF')
    alias = bridge.with_name('libwow64fex.so')
    if alias.read_bytes() != data:
        raise ValueError('Unix bridge aliases differ')
    modules[bridge.name] = {'sha256': sha(bridge), 'bytes': len(data), 'architecture': 'aarch64-ohos',
                            'aliases': ['libwow64fex.so']}
    value = {'schemaVersion': 1, 'lockSha256': sha(LOCK), 'upstreamCommit': lock['upstream']['commit'],
             'adaptedTree': lock['adaptedTree'], 'adaptedCommit': lock['adaptedCommit'],
             'modules': modules, 'source': str(source), 'seconds': round(time.monotonic() - started, 2),
             'deviceValidated': False, 'productionPinChanged': False}
    (build / 'fex-upstream-build.json').write_text(json.dumps(value, indent=2) + '\n')
    print(json.dumps(value, indent=2))


if __name__ == '__main__':
    main()
