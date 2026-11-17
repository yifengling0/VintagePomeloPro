#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Build the pinned Gamescope core for OHOS, without host dependency fallbacks.

The resulting shared library is a build milestone, not a runtime capability.
It is not copied into the application until its native transport is qualified.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

from build_gamescope_ohos_deps import ROOT, tools


def build(sdk, shared, jobs):
    work = ROOT/'build/gamescope/arm64-v8a'
    prefix, bindir, view = tools(sdk, shared, work/'deps')
    pcs = prefix/'lib/pkgconfig'
    pcs.mkdir(parents=True, exist_ok=True)
    (pcs/'vulkan.pc').write_text(
        f'prefix={sdk}/sysroot/usr\nlibdir=${{prefix}}/lib/aarch64-linux-ohos\n'
        'includedir=${prefix}/include\nName: Vulkan\nDescription: OHOS SDK Vulkan loader\n'
        'Version: 1.3.0\nLibs: -L${libdir} -lvulkan\nCflags: -I${includedir}\n')
    # wlroots' optional GLES renderer must bind the OHOS system API. Its
    # relocated metadata otherwise pulls the guest Mesa GLES library into the
    # compositor, introducing a second graphics runtime merely to load it.
    (pcs/'glesv2.pc').write_text(
        f'prefix={sdk}/sysroot/usr\nlibdir=${{prefix}}/lib/aarch64-linux-ohos\n'
        'includedir=${prefix}/include\nName: OHOS OpenGL ES\n'
        'Description: OHOS SDK GLES 3 API including GLES 2\n'
        'Version: 3.0\nLibs: -L${libdir} -lGLESv3\nCflags: -I${includedir}\n')
    cross = work/'ohos-cross.ini'
    cross.write_text(
        '[binaries]\n'+f'c = {str(bindir/"cc")!r}\ncpp = {str(bindir/"cxx")!r}\n'
        f'ar = {str(sdk/"llvm/bin/llvm-ar")!r}\nstrip = {str(sdk/"llvm/bin/llvm-strip")!r}\n'
        f'pkg-config = {str(bindir/"pkg-config")!r}\n'
        '[host_machine]\nsystem = \'ohos\'\ncpu_family = \'aarch64\'\ncpu = \'aarch64\'\nendian = \'little\'\n'
        '[properties]\nneeds_exe_wrapper = true\n'
        '[built-in options]\n'+f'c_args = [\'-D_GNU_SOURCE\', \'-fPIC\']\n'
        'cpp_args = [\'-D_GNU_SOURCE\', \'-fPIC\', \'-DGLM_ENABLE_EXPERIMENTAL\']\n'
        f'c_link_args = [\'-fuse-ld=lld\', \'-L{view}/lib\']\n'
        f'cpp_link_args = [\'-fuse-ld=lld\', \'-L{view}/lib\']\n')
    native = work/'native-tools.ini'
    host_tools = shared.parents[2]/'host-tools'
    # Reuse the already-pinned display vendor database, rather than downloading
    # another hwdata tree. libdisplay-info uses this as build-time data only.
    database = shared.parents[3]/'thirdparty/dxvk-modern/subprojects/libdisplay-info/pnp.ids'
    data_lock = json.loads((ROOT/'components/gamescope-ohos-deps.lock.json').read_text())['dataSources'][0]
    if database.stat().st_size != data_lock['bytes'] or hashlib.sha256(database.read_bytes()).hexdigest() != data_lock['sha256']:
        raise ValueError('Display vendor database identity mismatch')
    native_metadata = work/'native-pkgconfig'
    native_metadata.mkdir(exist_ok=True)
    (native_metadata/'hwdata.pc').write_text(
        f'pkgdatadir={database.parent}\nName: Fixed display vendor database\n'
        'Description: Pinned pnp.ids data for the libdisplay-info generator\nVersion: 0\n')
    native_pc = bindir/'host-pkg-config'
    native_pc.write_text('#!/usr/bin/env python3\nimport os, subprocess, sys\n'
                         f'env = dict(os.environ, PKG_CONFIG_LIBDIR={str(native_metadata)+":"+str(host_tools/"lib/pkgconfig")!r})\n'
                         'env.pop("PKG_CONFIG_PATH", None)\n'
                         'p = subprocess.run(["/usr/bin/pkg-config", *sys.argv[1:]], env=env, capture_output=True)\n'
                         f'sys.stdout.buffer.write(p.stdout.replace(b"/data/src/winehua/build/host-tools", {str(host_tools).encode()!r}))\n'
                         'sys.stderr.buffer.write(p.stderr)\nsys.exit(p.returncode)\n')
    native_pc.chmod(0o755)
    native.write_text('[binaries]\nhsl_spirv_optimizer = \'/usr/bin/spirv-opt\'\n'
                      f'pkg-config = {str(native_pc)!r}\n')
    env = dict(os.environ)
    env.pop('PKG_CONFIG_PATH', None)
    env['PKG_CONFIG_LIBDIR'] = '/nonexistent-vpp-host-pkgconfig'
    env['PATH'] = str(shared.parents[2]/'host-tools/bin')+':'+env['PATH']
    options = json.loads((ROOT/'components/gamescope-ohos.lock.json').read_text())['options']
    source_lock = json.loads((ROOT/'components/gamescope-source.lock.json').read_text())
    revision = next(r['revision'] for r in source_lock['sources'] if r['path'] == '.')
    options = [*options, '-Dsource_revision='+revision+'+vpp-ohos']
    output = work/'native-build'
    command = ['meson', 'setup', str(output), str(work/'source'), '--cross-file', str(cross),
               '--native-file', str(native), '--wrap-mode=nodownload', '--buildtype=release',
               '--prefix', str(work/'install'), *options]
    if (output/'meson-private/coredata.dat').exists():
        command.extend(['--reconfigure', '--clearcache'])
        # Meson validates -D against cached options before rereading a changed
        # meson_options.txt. Refresh that cache once when a recipe adds options;
        # preserve all compiled objects instead of wiping the build directory.
        known = {entry['name'] for entry in json.loads(
            (output/'meson-info/intro-buildoptions.json').read_text())}
        if 'source_revision' not in known:
            subprocess.run([arg for arg in command if not arg.startswith('-Dsource_revision=')],
                           env=env, check=True)
    subprocess.run(command, env=env, check=True)
    subprocess.run(['ninja', '-C', output, '-j'+str(jobs), '-k0', 'src/libgamescope_core.so'], env=env, check=True)
    subprocess.run(['meson', 'install', '-C', output, '--no-rebuild', '--only-changed', '--tags', 'vpp-gamescope'], env=env, check=True)
    library = work/'install/lib/libgamescope_core.so'
    subprocess.run([bindir/'cxx', '-std=c++20', '-O2', ROOT/'host_tests/gamescope_load_probe.cpp',
                    '-ldl', '-o', work/'gamescope-load-probe'], env=env, check=True)
    dynamic = subprocess.check_output([sdk/'llvm/bin/llvm-readelf', '-d', library], text=True)
    header = subprocess.check_output([sdk/'llvm/bin/llvm-readelf', '-h', library], text=True)
    if ('AArch64' not in header or any(x in dynamic for x in ('libc.so.6', 'libstdc++', 'ld-linux', '/data/src', '/shared'))):
        raise ValueError('Non-OHOS core library')
    report = {'platform': 'aarch64-linux-ohos', 'library': str(library),
              'sha256': hashlib.sha256(library.read_bytes()).hexdigest(),
              'sourceReceiptSha256': hashlib.sha256((work/'source-receipt.json').read_bytes()).hexdigest(),
              'bytes': library.stat().st_size, 'dynamic': dynamic, 'runtimeQualified': False}
    (work/'native-build.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps({k: report[k] for k in ('platform', 'library', 'sha256', 'bytes', 'runtimeQualified')}))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', type=Path, required=True)
    parser.add_argument('--host-prefix', type=Path, required=True)
    parser.add_argument('--jobs', type=int, default=4)
    args = parser.parse_args()
    build(args.sdk.resolve(), args.host_prefix.resolve(), args.jobs)
