#!/usr/bin/env python3
"""Build private GNU Mesa DRI and GLX Xwayland from existing verified sources.

Reuse the Steam experiment's source/dependency trees without modifying them.
Only the isolated Proton runner consumes these outputs.
"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess


def sha(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024*1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--mesa-build', type=Path, required=True)
    p.add_argument('--xwayland-build', type=Path, required=True)
    p.add_argument('--extra-deps', type=Path, required=True)
    p.add_argument('--meson', type=Path, required=True)
    p.add_argument('--runtime', type=Path, required=True, help='Verified shared GNU runtime; no machine-specific fallback')
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    mesa = a.mesa_build.resolve(strict=True)
    xwayland = a.xwayland_build.resolve(strict=True)
    extra = a.extra_deps.resolve(strict=True)
    output = a.output.resolve()
    if any(output.is_relative_to(source) for source in [mesa, xwayland]):
        p.error('Keep the experiment outside the shared source/build trees')
    output.mkdir(parents=True, exist_ok=True)
    # Generated XCB headers include their sibling headers with quotes. Share
    # the existing SDK headers in this private overlay instead of copying it.
    for source in (mesa/'deps-root/usr/include/xcb').glob('*.h'):
        target = extra/'usr/include/xcb'/source.name
        if not target.exists():
            target.symlink_to(source)
    meson = a.meson.resolve(strict=True)
    env = os.environ.copy()
    env['PATH'] = str(meson.parent) + ':' + env['PATH']
    env['PKG_CONFIG_PATH'] = ''
    pcs = [extra/'usr/lib/aarch64-linux-gnu/pkgconfig', mesa/'deps-root/usr/lib/aarch64-linux-gnu/pkgconfig',
           mesa/'deps-root/usr/share/pkgconfig']
    env['PKG_CONFIG_LIBDIR'] = ':'.join(map(str, pcs))
    cross = output/'proton-graphics-cross.ini'
    text = (mesa/'cross.ini').read_text()
    text = text.replace("pkg_config_libdir=[", "pkg_config_libdir=['" + str(pcs[0]) + "',")
    text = text.replace("c_args=[", "c_args=['-I" + str(extra/'usr/include') + "',")
    text = text.replace("cpp_args=[", "cpp_args=['-I" + str(extra/'usr/include') + "',")
    for key in ['c_args=[', 'cpp_args=[']:
        text = text.replace(key, key + "'-I" + str(mesa/'deps-root/usr/include/xcb') + "',")
    for key in ['c_link_args=[', 'cpp_link_args=[']:
        text = text.replace(key, key + "'-L" + str(extra/'usr/lib/aarch64-linux-gnu') +
                            "','-Wl,-rpath-link," + str(extra/'usr/lib/aarch64-linux-gnu') + "',")
    cross.write_text(text)

    def run(name, command):
        with (output/(name+'.log')).open('w') as log:
            result = subprocess.run(list(map(str, command)), env=env, stdout=log, stderr=subprocess.STDOUT)
        if result.returncode:
            raise RuntimeError(name + ': ' + (output/(name+'.log')).read_text()[-5000:])

    def configure(name, source, options, cross_file):
        build = output/name
        command = [meson, 'setup']
        if (build/'build.ninja').exists():
            command += ['--reconfigure']
        command += [build, source, '--cross-file='+str(cross_file), '--buildtype=release',
                    '--wrap-mode=nofallback', *options]
        run(name+'-configure', command)
        return build

    options = ['-Dplatforms=x11', '-Dgallium-drivers=softpipe,virgl', '-Dglx=dri',
               '-Dxlib-lease=disabled', '-Dglvnd=disabled', '-Dshared-glapi=enabled',
               '-Degl=enabled', '-Dgbm=enabled', '-Dllvm=disabled', '-Dvulkan-drivers=[]',
               '-Dgles1=disabled', '-Dgles2=disabled', '-Dgallium-vdpau=disabled',
               '-Dgallium-va=disabled', '-Dlibunwind=disabled', '-Dzstd=disabled',
               '-Dvalgrind=disabled', '-Dlmsensors=disabled', '-Dbuild-tests=false', '-Dtools=[]']
    build = configure('mesa-dri-build', mesa/'mesa-source', options, cross)
    run('mesa-dri-build', ['ninja', '-C', build, '-j4'])

    # Xwayland needs the actual Mesa build's GL and DRI SDK, not host headers.
    sdk = output/'graphics-pkgconfig'
    sdk.mkdir(exist_ok=True)
    src = mesa/'mesa-source'
    (sdk/'dri.pc').write_text('Name: dri\nDescription: Private GNU Mesa DRI SDK\nVersion: '+(src/'VERSION').read_text().strip()+'\n'
                            'dridriverdir=/data/storage/el2/base/files/steamohos-native/proton-probe\n'
                            'Cflags: -I'+str(src/'include')+'\n')
    (sdk/'gl.pc').write_text('Name: gl\nDescription: Private GNU Mesa OpenGL\nVersion: 1.2\n'
                           'Libs: -L'+str(build/'src/glx')+' -lGL\nCflags: -I'+str(src/'include')+'\n')
    xcross = output/'proton-xwayland-cross.ini'
    text = (xwayland/'cross.ini').read_text()
    # The historical build used an ephemeral compiler path. Resolve the
    # compiler from the reused Mesa build rather than recreating that path.
    compiler = re.search(r"^c = '([^']+)'", (mesa/'cross.ini').read_text(), re.M).group(1)
    for key, suffix in [('c', 'gcc'), ('ar', 'ar'), ('strip', 'strip')]:
        text = re.sub(r"^"+key+r" = '[^']+'", key+" = '"+compiler.removesuffix('gcc')+suffix+"'", text, flags=re.M)
    text = re.sub(r'pkg_config_libdir = \[.*?\]',
                  'pkg_config_libdir = '+repr([str(sdk), *map(str, pcs),
                    str(xwayland/'deps-root/usr/lib/aarch64-linux-gnu/pkgconfig'),
                    str(xwayland/'deps-root/usr/share/pkgconfig')]), text)
    xcross.write_text(text)
    env['PKG_CONFIG_LIBDIR'] = str(sdk)+':'+env['PKG_CONFIG_LIBDIR']
    # Reuse the native wayland-scanner already recorded by the original build.
    original_ninja = (xwayland/'build-xwayland/build.ninja').read_text()
    scanner = re.search(r'COMMAND = (\S+/bin/wayland-scanner) ', original_ninja)
    if not scanner:
        raise ValueError('Original native wayland-scanner not found')
    scanner = Path(scanner.group(1)).resolve(strict=True)
    env['PKG_CONFIG_PATH_FOR_BUILD'] = str(scanner.parent.parent/'lib/pkgconfig')
    xoptions = ['-Dglamor=false', '-Dglx=true', '-Ddrm=false', '-Ddri3=false', '-Dxvfb=false',
                '-Dxwayland_ei=false', '-Dlibdecor=false', '-Dsystemd_notify=false',
                '-Dinput_thread=false', '-Dxselinux=false', '-Dsecure-rpc=false', '-Dxdmcp=false',
                '-Ddocs=false', '-Ddevel-docs=false', '-Dsha1=libmd', '-Ddefault_font_path=built-ins',
                '-Dxkb_dir=/data/storage/el2/base/files/steamohos-native/xkb-data',
                '-Dxkb_output_dir=/nonexistent']
    link_paths = [build/'src/gallium/targets/dri', extra/'usr/lib/aarch64-linux-gnu',
                  mesa/'deps-root/usr/lib/aarch64-linux-gnu', mesa/'deps-root/lib/aarch64-linux-gnu',
                  xwayland/'deps-root/usr/lib/aarch64-linux-gnu']
    xoptions += ['-Dc_link_args='+repr(['-Wl,-rpath-link,'+':'.join(map(str, link_paths)),
                    *['-L'+str(path) for path in link_paths], '-ldl'])]
    xb = configure('xwayland-glx-build', xwayland/'xwayland-24.1.3', xoptions, xcross)
    # Meson caches compiler options from the original cross file. Apply the
    # link closure explicitly to the existing incremental build as well.
    run('xwayland-glx-link-configure', [meson, 'configure', xb, xoptions[-1]])
    run('xwayland-glx-build', ['ninja', '-C', xb, '-j4'])
    gallium = next((build/'src/gallium/targets/dri').glob('libgallium-*.so'))
    artifacts = {'Xwayland-glx': xb/'hw/xwayland/Xwayland', 'libGL-proton.so.1': build/'src/glx/libGL.so.1.2.0',
                 gallium.name: gallium, 'libdril_dri.so': build/'src/gallium/targets/dril/libdril_dri.so',
                 'libgbm.so.1': build/'src/gbm/libgbm.so.1.0.0',
                 'libEGL-proton.so.1': build/'src/egl/libEGL.so'}
    runtime = a.runtime.resolve(strict=True)
    shared = set((output/'runtime-modules.list').read_text().splitlines())
    search = [extra/'usr/lib/aarch64-linux-gnu', mesa/'deps-root/usr/lib/aarch64-linux-gnu',
              mesa/'deps-root/lib/aarch64-linux-gnu', xwayland/'deps-root/usr/lib/aarch64-linux-gnu', runtime]
    provided = {'libGL.so.1', gallium.name, 'libgbm.so.1'}
    queue = list(artifacts.values())
    closure = {}
    while queue:
        source = queue.pop()
        needed = re.findall(r'Shared library: \[([^]]+)\]', subprocess.check_output(
            ['readelf', '-d', str(source)], text=True))
        for name in needed:
            if name in provided or name in closure:
                continue
            library = next((directory/name for directory in search if (directory/name).is_file()), None)
            if library is None:
                raise ValueError('Unresolved graphics dependency: '+name+' from '+str(source))
            closure[name] = {'source': str(library.resolve()), 'sha256': sha(library),
                             'shared': name in shared}
            if name not in shared:
                artifacts[name] = library
            queue.append(library)
    records = {}
    for name, source in artifacts.items():
        header = source.read_bytes()[:64]
        if header[:4] != b'\x7fELF' or int.from_bytes(header[18:20], 'little') != 183:
            raise ValueError('Expected GNU AArch64 ELF: '+str(source))
        target = output/name
        if not target.exists() or sha(target) != sha(source):
            shutil.copyfile(source, target)
        records[name] = {'bytes': target.stat().st_size, 'sha256': sha(target)}
    (output/'proton-graphics-build.json').write_text(json.dumps({'files': records, 'dependencies': closure,
         'aliases': {'swrast_dri.so': 'libdril_dri.so'}, 'mesaOptions': options,
         'xwaylandOptions': xoptions, 'scope': 'isolated GNU Proton experiment'}, indent=2)+'\n')
    print(json.dumps(records, indent=2))


if __name__ == '__main__':
    main()
