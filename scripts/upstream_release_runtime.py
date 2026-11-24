#!/usr/bin/env python3
"""Enforce the branch's selected, tested upstream Wine/FEX bytes at packaging."""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
from zipfile import ZipFile

ROOT = Path(__file__).resolve().parents[1]
LOCK = ROOT / 'docs/upstream-sync/release-runtime.lock.json'
RAW = 'resources/rawfile/'


def digest(stream):
    value = hashlib.sha256()
    for chunk in iter(lambda: stream.read(1024 * 1024), b''):
        value.update(chunk)
    return value.hexdigest()


def file_sha(path):
    with path.open('rb') as stream:
        return digest(stream)


def load_lock():
    lock = json.loads(LOCK.read_text())
    if lock['schemaVersion'] != 1:
        raise ValueError('Unknown selected upstream runtime schema')
    for component in ('wine', 'fex'):
        item = lock[component]
        path = ROOT / item['lockFile']
        if file_sha(path) != item['lockSha256']:
            raise ValueError('Selected upstream source lock changed: ' + component)
        source = json.loads(path.read_text())
        if source['adaptedCommit'] != item['adaptedCommit']:
            raise ValueError('Selected upstream source identity changed: ' + component)
        if 'packageLockFile' in item:
            original = ROOT / item['packageLockFile']
            if file_sha(original) != item['packageLockSha256']:
                raise ValueError('Original packaged source lock changed: ' + component)
            previous = json.loads(original.read_text())
            if any(previous[k] != source[k] for k in ('adaptedCommit', 'adaptedTree')):
                raise ValueError('Reverified source differs from packaged source: ' + component)
    graphics = lock.get('graphics')
    if graphics:
        # The selected runtime retains the graphics provenance of its build.
        # Validate the live source lock separately when its inventory advances.
        source_sha = graphics.get('reviewedSourceLockSha256', graphics['sourceLockSha256'])
        for key, expected_sha in (('source', source_sha), ('texture', graphics['textureLockSha256'])):
            path = ROOT / graphics[key + 'LockFile']
            if file_sha(path) != expected_sha:
                raise ValueError('Selected graphics source lock changed: ' + key)
        source = json.loads((ROOT / graphics['sourceLockFile']).read_text())
        texture = json.loads((ROOT / graphics['textureLockFile']).read_text())
        patches = [row for rows in source['managed_patches'].values() for row in rows]
        patches += texture['venus']['patches']
        patches += texture['runtimes']['legacy']['patches']
        gpu_bc = texture['runtimes']['legacy'].get('gpuBcUpload')
        if gpu_bc:
            patches += [gpu_bc['patch'], gpu_bc['mesaPatch']]
        for row in patches:
            data = (ROOT / row['path']).read_bytes()
            # Historical pins used raw bytes; newer overlays normalize LF.
            if row['sha256'] not in {hashlib.sha256(data).hexdigest(),
                    hashlib.sha256(data.replace(b'\r\n', b'\n')).hexdigest()}:
                raise ValueError('Selected graphics patch changed: ' + row['path'])
    controller = lock.get('controller')
    if controller:
        if file_sha(ROOT / controller['patchFile']) != controller['patchSha256']:
            raise ValueError('Selected controller patch changed')
        header = (ROOT / 'entry/src/main/cpp/input/controller/gamepad_ipc_protocol.h').read_text()
        begin, end = '/* >>> WHGP_PROTOCOL_BEGIN */', '/* <<< WHGP_PROTOCOL_END */'
        block = header[header.index(begin):header.index(end) + len(end)]
        if hashlib.sha256(block.encode()).hexdigest() != controller['protocolBlockSha256']:
            raise ValueError('Selected controller host protocol changed')
    return lock


def verify_graphics(open_member, manifest, graphics):
    record = manifest.get('graphicsSync', {})
    for key in ('sourceLockSha256', 'textureLockSha256', 'qualification'):
        if record.get(key) != graphics[key]:
            raise ValueError('Unpaired graphics runtime metadata: ' + key)
    from package_texture_compat_runtime import validate
    from source_build_contract import elf_identity
    texture = json.loads((ROOT / graphics['textureLockFile']).read_text())
    with open_member(RAW + 'wine-data.zip') as stream:
        payload = stream.read()
    with ZipFile(io.BytesIO(payload)) as runtime:
        guest = runtime.read('bin/guest_vulkan/lib/libvulkan_virtio.so')
        validate(runtime, guest, texture)
        dxvk = json.loads(runtime.read('dxvk/manifest.json'))
        gpu_bc = texture['runtimes']['legacy'].get('gpuBcUpload')
        if graphics.get('gpuBcUpload'):
            with open_member(RAW + 'gpu-bc-upload.flag') as flag:
                if flag.read() != b'1':
                    raise ValueError('Invalid GPU BC runtime capability marker')
            mesa = json.loads(runtime.read('bin/guest_gfx/guest-gfx-build.json'))
            if (dxvk['runtimes']['legacy'].get('gpuBcUpload') != gpu_bc['patch'] or
                    mesa['mesaSource'].get('gpuBcUpload') != gpu_bc['mesaPatch']):
                raise ValueError('Unpaired GPU BC Mesa and Legacy DXVK build')
    with open_member('libs/arm64-v8a/libvulkan_virtio.so') as stream:
        if elf_identity(stream.read()) != elf_identity(guest):
            raise ValueError('Unpaired top-level and nested graphics ICD')
    with open_member('libs/arm64-v8a/libvirglrenderer.so.1') as stream:
        if digest(stream) != dxvk['venusRuntime']['hostVirglrendererSha256']:
            raise ValueError('Unpaired graphics host renderer')


def verify(open_member, names, lock):
    names = set(names)
    for name, expected in lock['members'].items():
        if name not in names:
            raise ValueError('Selected upstream runtime member missing: ' + name)
        with open_member(name) as stream:
            if digest(stream) != expected['sha256']:
                raise ValueError('Selected upstream runtime bytes differ: ' + name)
    with open_member(RAW + 'wine-runtime-manifest.json') as stream:
        manifest = json.load(stream)
    for component in ('wine', 'fex'):
        actual = manifest.get('componentOverrides', {}).get(component, {})
        if actual.get('adaptedCommit') != lock[component]['adaptedCommit']:
            raise ValueError('Old or mixed upstream runtime selected: ' + component)
        expected_lock = lock[component].get('packageLockSha256', lock[component]['lockSha256'])
        if actual.get('lockSha256') != expected_lock:
            raise ValueError('Upstream runtime source lock mismatch: ' + component)
    if lock.get('graphics'):
        verify_graphics(open_member, manifest, lock['graphics'])
    if lock.get('controller'):
        if manifest.get('controllerRuntime') != lock['controller']['runtimeMetadata']:
            raise ValueError('Unpaired controller runtime metadata')
    return dict(wine=lock['wine']['adaptedCommit'], fex=lock['fex']['adaptedCommit'],
                verifiedMembers=len(lock['members']))


def verify_hap(path, lock=None):
    with ZipFile(path) as archive:
        if len(archive.namelist()) != len(set(archive.namelist())):
            raise ValueError('Duplicate upstream HAP members')
        return verify(archive.open, archive.namelist(), lock or load_lock())


def staged_path(name):
    if name.startswith('libs/arm64-v8a/'):
        return ROOT / 'entry' / name
    if name.startswith(RAW):
        return ROOT / 'entry/src/main/resources/rawfile' / name.removeprefix(RAW)
    raise ValueError('Unregistered upstream staging path: ' + name)


def verify_staged(lock):
    return verify(lambda n: staged_path(n).open('rb'), lock['members'], lock)


def stage(hap, lock):
    # Validate every member before changing staging. Never follow a generated
    # symlink into a shared compiler output when replacing a component.
    verify_hap(hap, lock)
    with ZipFile(hap) as archive:
        for name, expected in lock['members'].items():
            path = staged_path(name)
            if path.is_file() and file_sha(path) == expected['sha256']:
                continue
            path.parent.mkdir(parents=True, exist_ok=True)
            pending = path.with_name(path.name + '.selected-upstream.tmp')
            try:
                with archive.open(name) as src, pending.open('wb') as dst:
                    shutil.copyfileobj(src, dst, 1024 * 1024)
                if file_sha(pending) != expected['sha256']:
                    raise ValueError('Upstream staging copy failed: ' + name)
                os.replace(pending, path)
            finally:
                pending.unlink(missing_ok=True)
    return verify_staged(lock)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument('--hap', type=Path)
    group.add_argument('--stage', type=Path)
    group.add_argument('--staged', action='store_true')
    args = parser.parse_args()
    lock = load_lock()
    result = stage(args.stage, lock) if args.stage else verify_hap(args.hap, lock) if args.hap else verify_staged(lock)
    print(json.dumps(dict(status='PASS', **result)))


if __name__ == '__main__':
    main()
