"""Reject the actual mixed-runtime regression and independently stale inputs."""
import argparse
import io
import json
from pathlib import Path
import sys
from zipfile import ZipFile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from package_texture_compat_runtime import validate as validate_texture
from runtime_core_contract import validate_core
from stamp_wine_builtin_identity import validate_archive

parser = argparse.ArgumentParser()
parser.add_argument('--hap', type=Path, required=True)
parser.add_argument('--regressed-hap', type=Path, required=True)
args = parser.parse_args()


def rejected(call, fragment):
    try:
        call()
    except ValueError as error:
        assert fragment in str(error), error
    else:
        raise AssertionError('Accepted known regression: ' + fragment)


class Overlay:
    def __init__(self, source, replacements):
        self.source, self.replacements = source, replacements

    def read(self, name):
        return self.replacements[name] if name in self.replacements else self.source.read(name)


with ZipFile(args.hap) as hap, ZipFile(args.regressed_hap) as bad_hap:
    with ZipFile(io.BytesIO(hap.read('resources/rawfile/wine-data.zip'))) as wine, \
            ZipFile(io.BytesIO(bad_hap.read('resources/rawfile/wine-data.zip'))) as bad_wine:
        icd = hap.read('libs/arm64-v8a/libvulkan_virtio.so')
        validate_texture(wine, icd)
        validate_core(hap.read)
        validate_core(wine.read, 'payload')
        validate_archive(wine)
        rejected(lambda: validate_texture(bad_wine, bad_hap.read('libs/arm64-v8a/libvulkan_virtio.so')),
                 'unpaired DXVK commit')
        path = 'dxvk/legacy/x86/d3d11.dll'
        rejected(lambda: validate_texture(Overlay(wine, {path: bad_wine.read(path)}), icd),
                 'stale DXVK texture runtime')
        rejected(lambda: validate_texture(wine, bad_hap.read('libs/arm64-v8a/libvulkan_virtio.so')),
                 'unpaired top-level and nested Venus ICD')
        path = 'bin/guest_vulkan/lib/libvulkan_virtio.so'
        rejected(lambda: validate_texture(Overlay(wine, {path: bad_wine.read(path)}), icd),
                 'unpaired top-level and nested Venus ICD')
        path = 'libs/arm64-v8a/libwinehua_vtest_server.so'
        rejected(lambda: validate_core(Overlay(hap, {path: bad_hap.read(path)}).read),
                 'Unreviewed core runtime replacement')
        path = 'bin/i386-windows/ntdll.dll'
        rejected(lambda: validate_core(Overlay(wine, {path: wine.read(path) + b'stale'}).read, 'payload'),
                 'Unreviewed core runtime replacement')
        rejected(lambda: validate_archive(bad_wine), 'builtin DLL identity is stale')
        lock = json.loads((ROOT / 'docs/graphics/runtime-core.lock.json').read_text())
print(f'Runtime compatibility PASS: {len(lock["libraries"])} native / '
      f'{len(lock["payload"])} PE pins; actual stale DXVK, both ICDs, vtest and prefix identity rejected')
