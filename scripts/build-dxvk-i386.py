#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Build the pinned WineHua dxgi/d3d11 pair for PE32, without Wine Unix objects.

Uses the same unmodified fork inputs as the ARM64X builder, but the upstream
i686/SSE2 target and Win32 stdcall exports. A static inventory is not runtime
qualification: the installed component still requires its real ABI/WSI probe.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


common = module('dxvk_native_build', ROOT/'scripts/build-dxvk-arm64x.py')
pe = module('dxvk_pe', ROOT/'scripts/dxvk_pe.py')
sha, canonical, tree, run = common.sha, common.canonical, common.tree, common.run


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--variant', choices=tuple(common.COMMITS), required=True)
    for name in ('source', 'source-inputs', 'mingw', 'glslang'):
        p.add_argument('--'+name, type=Path, required=True)
    p.add_argument('--lock', type=Path, default=ROOT/'docs/linux-steam/gnu-dxvk-source.lock.json')
    p.add_argument('--cache', type=Path, default=ROOT/'build/gnu-dxvk/build/i386')
    p.add_argument('--output', type=Path, default=ROOT/'build/gnu-dxvk/results')
    p.add_argument('--jobs', type=int, default=4)
    a = p.parse_args()
    if not 1 <= a.jobs <= 8:
        p.error('jobs must be 1..8')
    src, mgw, glslang = (v.resolve(strict=True) for v in (a.source, a.mingw, a.glslang))
    locked = json.loads(a.lock.read_text())
    acquired = json.loads(a.source_inputs.read_text())
    modern = a.variant.startswith('modern')
    key = 'modern' if modern else 'legacy'
    files = tree(src)
    source_row = next(r for r in acquired['sourceInputs'] if r['name'] == key)
    expected = {r['path']: r['sha256'] for r in acquired['sourceTrees'][key]['files']}
    source_digest = hashlib.sha256(canonical(files)).hexdigest()
    if (locked.get('schemaVersion') != 2 or files != expected or
            source_row['commit'] != common.COMMITS[a.variant] or
            locked['variants'][a.variant]['source']['commit'] != common.COMMITS[a.variant] or
            locked['variants'][a.variant]['source']['materializedTreeSha256'] != source_digest):
        raise ValueError('Source tree differs from pinned acquisition/lock')
    bcdec = src.parent/'wine/dlls/d3dx9_36/bcdec.h'
    if sha(bcdec) != locked['buildInputs']['bcdec']['sha256']:
        raise ValueError('Unpinned BC decoder')
    binaries = {n: mgw/'bin'/n for n in ('clang', 'ld.lld', 'llvm-ar', 'llvm-dlltool', 'llvm-readobj')}
    for name, path in binaries.items():
        if sha(path) != locked['buildInputs']['llvmMingw']['binarySha256'][name]:
            raise ValueError('Unpinned LLVM tool: '+name)
    if sha(glslang) != locked['buildInputs']['hostBinaries']['glslangValidator']['sha256']:
        raise ValueError('Unpinned shader compiler')
    libroot = mgw/'i686-w64-mingw32/lib'
    libraries = {n: libroot/n for n in ('libc++abi.a', 'libunwind.a', 'libc++.a')}
    for name, path in libraries.items():
        if sha(path) != locked['buildInputs']['llvmMingw']['i386StaticLibrarySha256'][name]:
            raise ValueError('Unpinned i386 static runtime: '+name)
    includes = [src/'src', src/'include']
    includes += ([src/'include/vulkan/include', src/'include/spirv/include', src/'include/native',
                  src/'subprojects/libdisplay-info/include'] if modern else
                 [src/'include/spirv', src/'include/vulkan'])
    target = ['--target=i686-w64-mingw32', '-msse', '-msse2', '-mfpmath=sse']
    base = ['-O2', '-DNDEBUG', '-DNOMINMAX', '-D_WIN32_WINNT=0xa00']
    cpp = ['-std=c++17']+base+(['-DDXVK_WSI_WIN32'] if modern else [])
    c = ['-std=c11']+base+(['-D_POSIX_C_SOURCE=200809L', '-Dstatic_array=static'] if modern else [])
    inputs = dict(schema=1, variant=a.variant, sourceCommit=common.COMMITS[a.variant], sourceFiles=files,
                  bcdecSha256=sha(bcdec), sourceInputsSha256=sha(a.source_inputs),
                  builderSha256=sha(Path(__file__)), helperSha256=sha(Path(common.__file__)),
                  toolchain={n: dict(sha256=sha(f), version=common.version(f)) for n, f in binaries.items()},
                  staticLibraries={n: sha(f) for n, f in libraries.items()},
                  glslang=dict(sha256=sha(glslang), version=common.version(glslang)),
                  target=target, cppFlags=cpp, cFlags=c, libraries=common.LIBRARIES,
                  exportOrdinals=common.ORDINALS,
                  vulkanImportBoundary='i386 public vkGetInstanceProcAddr only; no Wine Unix objects',
                  linkProfile='dxgi+d3d11; original .def ordinals and --kill-at')
    fingerprint = hashlib.sha256(canonical(inputs)).hexdigest()
    cache = a.cache/a.variant/fingerprint
    gen, obj, logs = (cache/n for n in ('gen', 'obj', 'logs'))
    out = a.output/a.variant/'i386'
    bindir = out/'bin'
    for d in (gen, obj, logs, bindir):
        d.mkdir(parents=True, exist_ok=True)
    (cache/'inputs.json').write_bytes(canonical(inputs))
    (gen/'version.h').write_text((src/'version.h.in').read_text().replace('@VCS_TAG@', 'v2.6' if modern else 'v1.10.3'))
    names = {'version.h'}
    if modern:
        (gen/'buildenv.h').write_text('#pragma once\n#define DXVK_TARGET "x86"\n#define DXVK_COMPILER "clang"\n#define DXVK_COMPILER_VERSION "23"\n')
        names.add('buildenv.h')
    for folder in ('src/dxvk/shaders', 'src/dxvk/hud/shaders', 'src/d3d11/shaders'):
        for source in sorted((src/folder).rglob('*')):
            if source.suffix not in ('.vert', '.frag', '.comp', '.geom'):
                continue
            name = source.stem+'.h'
            if name in names:
                raise ValueError('Generated header collision')
            common.generate_shader(glslang, source, gen/name, logs/(name+'.log'))
            names.add(name)
    if modern:
        run(['python3', src/'subprojects/libdisplay-info/tool/gen-search-table.py',
             src/'subprojects/libdisplay-info/pnp.ids', gen/'pnp-id-table.c', 'pnp_id_table'], logs/'pnp.log')
        names.add('pnp-id-table.c')
    generated = {n: sha(gen/n) for n in sorted(names)}
    generated_digest = hashlib.sha256(canonical(generated)).hexdigest()
    folders = ('util', 'spirv', 'vulkan', 'dxvk', 'dxbc', 'dxgi', 'd3d10', 'd3d11')+(('wsi',) if modern else ())
    sources = [(f, f.relative_to(src/'src').as_posix().replace('/', '_')+'.o')
               for folder in folders for f in sorted((src/'src'/folder).rglob('*')) if f.suffix in ('.cpp', '.c')]
    if modern:
        sources += [(f, 'libdisplay-info_'+f.name+'.o') for f in sorted((src/'subprojects/libdisplay-info').glob('*.c'))]
        sources += [(gen/'pnp-id-table.c', 'pnp-id-table.c.o')]
    if len({n for _, n in sources}) != len(sources):
        raise ValueError('Object name collision')

    def compile_one(row):
        source, name = row
        output = obj/name
        receipt = output.with_suffix('.json')
        if common.reusable_object(output, receipt, source, generated_digest):
            return
        run([binaries['clang'], *target, *(c if source.suffix == '.c' else cpp),
             *('-I'+str(v) for v in includes+[gen]), '-c', source, '-o', str(output)+'.partial'], logs/(name+'.log'))
        os.replace(str(output)+'.partial', output)
        receipt.write_bytes(canonical(dict(sha256=sha(output), sourceSha256=sha(source), generatedInputsSha256=generated_digest)))

    print(json.dumps(dict(stage='compile', architecture='i386', variant=a.variant, sources=len(sources), fingerprint=fingerprint)), flush=True)
    with ThreadPoolExecutor(max_workers=a.jobs) as pool:
        futures = [pool.submit(compile_one, row) for row in sources]
        for index, future in enumerate(as_completed(futures), 1):
            future.result()
            if index % 25 == 0:
                print(json.dumps(dict(stage='compile', complete=index, total=len(sources))), flush=True)
    definition = gen/'vulkan.def'
    # Vulkan loader functions use VKAPI_CALL (__stdcall) on Win32.
    definition.write_text('LIBRARY vulkan-1.dll\nEXPORTS\n    vkGetInstanceProcAddr@8\n')
    vulkan = cache/'libvulkan-1.a'
    run([binaries['llvm-dlltool'], '-m', 'i386', '-k', '-d', definition, '-l', vulkan], logs/'vulkan-import.log')
    results = {}
    with tempfile.TemporaryDirectory(prefix='.hsl-i386-link-', dir=out) as directory:
        stage = Path(directory)
        for name in ('dxgi', 'd3d11'):
            keep = ['util_', 'spirv_', 'vulkan_', 'wsi_', 'dxvk_']
            keep += ['dxgi_'] if name == 'dxgi' else ['dxgi_dxgi_format', 'dxbc_', 'd3d10_', 'd3d11_']
            if not modern and name == 'd3d11':
                keep += ['dxgi_dxgi_monitor', 'dxgi_dxgi_swapchain']
            objects = [obj/n for _, n in sources if not n.startswith('d3d10_d3d10_main') and
                       (any(n.startswith(k) for k in keep) or
                        (modern and n.startswith(('libdisplay-info_', 'pnp-id-table'))))]
            exports = common.exports((src/f'src/{name}/{name}.def').read_text(), name)
            binary = stage/(name+'.dll')
            command = [binaries['clang'], *target, '-shared', '-o', binary,
                       '-Wl,--kill-at', '-Wl,--no-insert-timestamp',
                       '-Wl,--out-implib='+str(cache/(name+'.lib')), src/f'src/{name}/{name}.def', *objects]
            if name == 'd3d11':
                command += [cache/'dxgi.lib']
            command += [vulkan, '-L'+str(libroot), '-Wl,--start-group', *libraries.values(), '-Wl,--end-group',
                        *('-l'+n for n in common.LIBRARIES)]
            print(json.dumps(dict(stage='link', module=name, objects=len(objects))), flush=True)
            run(command, logs/(name+'.link.log'))
            inventory = pe.validate_dxvk(binary.read_bytes(), name+'.dll', architecture='i386')
            imported = inventory['graphs']['primary']['importModules']
            if any(re.search(r'libc\+\+|libunwind', n, re.I) for n in imported):
                raise ValueError('Dynamic C++ dependency')
            if modern and b'DXVK_WSI_DRIVER environment variable unset' in binary.read_bytes():
                raise ValueError('Modern Win32 WSI not selected')
            results[name+'.dll'] = dict(bytes=binary.stat().st_size, sha256=sha(binary),
                                       expectedExports=exports, objectCount=len(objects), pe=inventory)
        if (tree(src) != files or sha(bcdec) != inputs['bcdecSha256'] or
                sha(Path(__file__)) != inputs['builderSha256'] or
                sha(Path(common.__file__)) != inputs['helperSha256'] or
                {n: sha(gen/n) for n in names} != generated):
            raise ValueError('Build input changed during compilation')
        for name in results:
            os.replace(stage/name, bindir/name)
    receipt = dict(schema=2, variant=a.variant, architecture='i386', sourceCommit=common.COMMITS[a.variant],
                   sourceTreeSha256=source_digest, inputsSha256=fingerprint, inputs=inputs,
                   generatedInputsSha256=generated_digest, generatedInputs=generated, files=results,
                   inventoryHelperSha256=sha(ROOT/'scripts/dxvk_pe.py'), publicVulkanImportSha256=sha(vulkan),
                   runtimeExecuted=False, profileScope='PE32 dxgi+d3d11 only; no D3D9 or complete native D3D10 profile')
    (out/'build.json').write_text(json.dumps(receipt, indent=2)+'\n')
    print(json.dumps(dict(stage='complete', architecture='i386', variant=a.variant,
                          files={n: r['sha256'] for n, r in results.items()})), flush=True)


if __name__ == '__main__':
    main()
