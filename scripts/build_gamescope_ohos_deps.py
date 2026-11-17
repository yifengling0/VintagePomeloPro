#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Build the missing X11 client libraries with the OHOS SDK, in a private prefix.

Shared VPP host dependencies are read-only. No host/GNU libraries are used as
target inputs. Invoke through the Makefile in winehua-dev.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]


def run(args, **kwargs):
    subprocess.run(list(map(str, args)), check=True, **kwargs)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def tools(sdk, shared, work):
    """Remap metadata output, never mutate shared .pc files or search host dirs."""
    bindir = work/'tools'
    prefix = work/'usr'
    bindir.mkdir(parents=True, exist_ok=True)
    # Libtool follows dependency_libs in relocated shared .la files. Expose
    # shared objects only in our link view instead of modifying those archives.
    view = work/'host-view'
    (view/'lib').mkdir(parents=True, exist_ok=True)
    for part in ('include', 'share'):
        if not (view/part).exists():
            (view/part).symlink_to(shared/part, target_is_directory=True)
    for library in [*(shared/'lib').glob('*.so*'), *(shared/'lib').glob('*.a')]:
        if not (view/'lib'/library.name).exists():
            (view/'lib'/library.name).symlink_to(library)
    metadata = ':'.join(str(base/part) for base in (prefix, shared)
                        for part in ('lib/pkgconfig', 'share/pkgconfig'))
    native_prefix = shared.parents[2]/'native_arm64-v8a'
    metadata += ':'+str(native_prefix/'libffi/install/lib/pkgconfig')
    # Protocol XML is architecture-independent. Do not add the guest lib/pc
    # directories, which may contain x86_64 binaries.
    protocol_pc = shared.parents[2]/'sysroot-ext/usr/share/pkgconfig/wayland-protocols.pc'
    (prefix/'lib/pkgconfig').mkdir(parents=True, exist_ok=True)
    if protocol_pc.is_file():
        (prefix/'lib/pkgconfig/wayland-protocols.pc').write_text(protocol_pc.read_text().replace(
            '/data/src/winehua/build/sysroot-ext/usr', str(shared.parents[2]/'sysroot-ext/usr')))
    pc = bindir/'pkg-config'
    pc.write_text('#!/usr/bin/env python3\nimport os, subprocess, sys\n'
                  'env = dict(os.environ)\nenv.pop("PKG_CONFIG_PATH", None)\n'
                  f'env["PKG_CONFIG_LIBDIR"] = {metadata!r}\n'
                  'p = subprocess.run(["/usr/bin/pkg-config", *sys.argv[1:]], '
                  'env=env, capture_output=True)\n'
                  f'data = p.stdout.replace(b"/data/src/winehua/build/host-ext/arm64-v8a/usr", {str(view).encode()!r}).replace({str(shared).encode()!r}, {str(view).encode()!r})\n'
                  f'data = data.replace(b"/data/src/winehua/build/native_arm64-v8a", {str(native_prefix).encode()!r})\n'
                  'sys.stdout.buffer.write(data)\n'
                  'sys.stderr.buffer.write(p.stderr)\nsys.exit(p.returncode)\n')
    pc.chmod(0o755)
    for name, compiler in [('cc', 'clang'), ('cxx', 'clang++')]:
        script = bindir/name
        script.write_text('#!/bin/sh\nexec '+shlex.quote(str(sdk/'llvm/bin'/compiler))+
                          ' --target=aarch64-linux-ohos --sysroot='+
                          shlex.quote(str(sdk/'sysroot'))+' "$@"\n')
        script.chmod(0o755)
    return prefix, bindir, view


def extract(archive, destination, root):
    # Release archives contain ordinary files/directories only; reject links,
    # absolute names, traversal and unexpectedly large expansions.
    with tarfile.open(archive) as package:
        members = package.getmembers()
        if len(members) > 5000 or sum(m.size for m in members) > 32*1024**2:
            raise ValueError('X11 source expansion limit exceeded')
        for m in members:
            parts = Path(m.name).parts
            if (not parts or parts[0] != root or '..' in parts or
                    Path(m.name).is_absolute() or not (m.isfile() or m.isdir())):
                raise ValueError('Unsupported X11 source member: '+m.name)
        package.extractall(destination, members=members, filter='data')


def build(sdk, shared, work, cache, jobs):
    lock = json.loads((ROOT/'components/gamescope-ohos-deps.lock.json').read_text())
    prefix, bindir, view = tools(sdk, shared, work)
    env = dict(os.environ, CC=str(bindir/'cc'), CXX=str(bindir/'cxx'),
               AR=str(sdk/'llvm/bin/llvm-ar'), RANLIB=str(sdk/'llvm/bin/llvm-ranlib'),
               PKG_CONFIG=str(bindir/'pkg-config'),
               CFLAGS=f'-O2 -fPIC -I{prefix}/include -I{shared}/include',
               LDFLAGS=f'-fuse-ld=lld -L{prefix}/lib -L{view}/lib')
    env.pop('PKG_CONFIG_PATH', None)
    env['PKG_CONFIG_LIBDIR'] = ''
    records = []
    for source in lock['sources']:
        archive = cache/source['archive']
        if archive.stat().st_size != source['bytes'] or digest(archive) != source['sha256']:
            raise ValueError('Dependency source mismatch: '+source['archive'])
        name = source['name']+'-'+source['version']
        src = work/'sources'/name
        receipt = src/'.vpp-build-receipt.json'
        identity = {'source': source, 'script': digest(Path(__file__)), 'sdk': str(sdk),
                    'shared': str(shared)}
        libraries = {'libXdamage': 'libXdamage.so.1', 'libXtst': 'libXtst.so.6',
                     'libXres': 'libXRes.so.1', 'libXmu': 'libXmuu.so.1'}
        library = prefix/'lib'/libraries[source['name']]
        if not (receipt.is_file() and json.loads(receipt.read_text()) == identity and library.is_file()):
            if not src.is_dir():
                src.parent.mkdir(parents=True, exist_ok=True)
                extract(archive, src.parent, name)
            options = [src/'configure', '--host=aarch64-linux-gnu', '--build=x86_64-linux-gnu',
                       '--prefix='+str(prefix), '--libdir='+str(prefix/'lib'),
                       '--disable-static', '--enable-shared', '--disable-silent-rules',
                       '--disable-specs', '--disable-docs', '--enable-malloc0returnsnull=yes']
            if source['name'] == 'libXmu':
                # CurUtil.c belongs to upstream libXmuu; full Xmu adds Xt/ICE/SM
                # for unrelated widgets. Gamescope uses XmuCursorNameToIndex only.
                # Upstream configure unconditionally requires Xt even when only
                # building Xmuu. Compile its complete five-file source list.
                prefix.joinpath('lib/pkgconfig').mkdir(parents=True, exist_ok=True)
                sources = ['ClientWin.c', 'CursorName.c', 'DefErrMsg.c', 'GetHost.c', 'Lower.c']
                flags = subprocess.check_output([bindir/'pkg-config', '--cflags', '--libs', 'x11'], text=True)
                run([bindir/'cc', '-O2', '-fPIC', '-shared', '-Wl,-soname,libXmuu.so.1',
                     '-Wl,--no-undefined', '-I'+str(src/'include'),
                     *[src/'src'/f for f in sources], *shlex.split(flags), '-o', library], env=env)
                link = library.parent/'libXmuu.so'
                if not link.exists():
                    link.symlink_to(library.name)
                shutil.copytree(src/'include/X11/Xmu', prefix/'include/X11/Xmu', dirs_exist_ok=True)
                (prefix/'lib/pkgconfig/xmuu.pc').write_text(
                    f'prefix={prefix}\nlibdir=${{prefix}}/lib\nincludedir=${{prefix}}/include\n'
                    f'Name: Xmuu\nDescription: Xmu utilities without Xt\nVersion: {source["version"]}\n'
                    'Requires: x11\nLibs: -L${libdir} -lXmuu\nCflags: -I${includedir}\n')
            else:
                run(options, cwd=src, env=env)
                run(['make', '-j'+str(jobs)], cwd=src, env=env)
                run(['make', 'install'], cwd=src, env=env)
            receipt.write_text(json.dumps(identity, indent=2)+'\n')
        header = subprocess.check_output([sdk/'llvm/bin/llvm-readelf', '-h', library], text=True)
        dynamic = subprocess.check_output([sdk/'llvm/bin/llvm-readelf', '-d', library], text=True)
        if 'AArch64' not in header or any(x in dynamic for x in ('libc.so.6', 'libstdc++', 'ld-linux')):
            raise ValueError('Non-OHOS target library: '+str(library))
        records.append({'name': source['name'], 'library': str(library),
                        'sha256': digest(library), 'dynamic': dynamic})
    report = {'platform': 'aarch64-linux-ohos', 'runtimeQualified': False, 'libraries': records}
    (work/'deps-build.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps({'prefix': str(prefix), 'libraries': len(records), 'runtimeQualified': False}))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', type=Path, required=True)
    parser.add_argument('--host-prefix', type=Path, required=True)
    parser.add_argument('--work', type=Path, default=ROOT/'build/gamescope/arm64-v8a/deps')
    parser.add_argument('--cache', type=Path, default=ROOT/'build/gamescope/cache')
    parser.add_argument('--jobs', type=int, default=4)
    args = parser.parse_args()
    build(args.sdk.resolve(), args.host_prefix.resolve(), args.work.resolve(), args.cache.resolve(), args.jobs)
