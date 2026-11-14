"""Check real packaged graphics and reject relabelled, stale components."""
import argparse
import copy
import io
import json
from pathlib import Path
import sys
from zipfile import ZipFile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from upstream_release_runtime import RAW, load_lock, verify_graphics

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--hap', type=Path, required=True)
args = parser.parse_args()
lock = load_lock()['graphics']
texture = json.loads((ROOT / lock['textureLockFile']).read_text())
with ZipFile(args.hap) as hap:
    manifest = json.loads(hap.read(RAW + 'wine-runtime-manifest.json'))
    libraries = {n: hap.read(n) for n in (
        'libs/arm64-v8a/libvulkan_virtio.so', 'libs/arm64-v8a/libvirglrenderer.so.1')}
    with ZipFile(io.BytesIO(hap.read(RAW + 'wine-data.zip'))) as runtime:
        names = {'dxvk/manifest.json', 'bin/guest_vulkan/manifest.json',
                 'bin/guest_vulkan/lib/libvulkan_virtio.so'}
        names |= {'dxvk/' + profile + '/' + n
                  for profile, pin in texture['runtimes'].items() for n in pin['files']}
        baseline = {n: runtime.read(n) for n in names}

for case in ('valid', 'legacy', 'modern', 'guest', 'top-guest', 'host', 'qualification', 'lock'):
    nested = dict(baseline)
    data = dict(libraries)
    meta = copy.deepcopy(manifest)
    changed = {'legacy': 'dxvk/legacy/x86/d3d11.dll',
               'modern': 'dxvk/modern-2.6/arm64x/d3d11.dll',
               'guest': 'bin/guest_vulkan/lib/libvulkan_virtio.so'}.get(case)
    if changed:
        nested[changed] += b'stale-component'
    if case == 'top-guest':
        # A valid ELF with a different loadable identity, not just debug bytes.
        data['libs/arm64-v8a/libvulkan_virtio.so'] = data['libs/arm64-v8a/libvirglrenderer.so.1']
    if case == 'host':
        data['libs/arm64-v8a/libvirglrenderer.so.1'] += b'stale-host'
    if case == 'qualification':
        meta['graphicsSync']['qualification'] = 'device-tested'
    if case == 'lock':
        meta['graphicsSync']['textureLockSha256'] = '0' * 64
    buffer = io.BytesIO()
    with ZipFile(buffer, 'w') as runtime:
        for name, value in nested.items():
            runtime.writestr(name, value)
    data[RAW + 'wine-data.zip'] = buffer.getvalue()
    try:
        verify_graphics(lambda n: io.BytesIO(data[n]), meta, lock)
    except ValueError:
        assert case != 'valid', 'real packaged baseline must pass'
    else:
        assert case == 'valid', 'stale or relabelled component was accepted: ' + case
    print('selected graphics:', case, 'PASS')
