"""Keep the legacy VirGL fixes and the OHOS queue fix in the same WineD3D build."""
import hashlib
import argparse
import json
from pathlib import Path
import re
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
MODULES = {'i386-windows': 0x14c, 'aarch64-windows': 0xaa64}
PATCHES = (
    '0036-wined3d-pixel-format-id-lookup.patch',
    '0038-wined3d-discard-map-invalidation.patch',
    '0039-wined3d-opt-in-dynamic-buffer-sysmem.patch',
    '0049-wined3d-require-srgb-write-control-for-shared-storage.patch',
    '0053-wined3d-restore-legacy-alpha-test-state.patch',
    '0068-wined3d-ohos-command-event-wake.patch',
)
SOURCE_FILES = ('adapter_gl.c', 'buffer.c', 'cs.c', 'glsl_shader.c', 'resource.c', 'swapchain.c')


def sha(data):
    return hashlib.sha256(data).hexdigest()


def patch_records():
    return {name: sha((ROOT / 'patches/wine' / name).read_bytes().replace(b'\r\n', b'\n'))
            for name in PATCHES}


def source_contract(source):
    """Check actual files, including source snapshots nested inside another Git repo."""
    registered = re.findall(r'^ensure_wine_patch "\$SCRIPT_DIR/\.\./patches/wine/([^"\n]+)"',
                           (ROOT / 'scripts/build_wine.sh').read_text(), re.M)
    required = {name for name in registered if '-wined3d-' in name}
    if required != set(PATCHES):
        raise ValueError('WineD3D contract must cover every registered WineD3D overlay')
    for name in PATCHES:
        result = subprocess.run(['patch', '-d', str(source), '-p1', '--batch', '--force',
                                 '--fuzz=0', '--dry-run', '--reverse', '-i',
                                 str(ROOT / 'patches/wine' / name)], capture_output=True, timeout=15)
        if result.returncode:
            raise ValueError('WineD3D effective source is missing overlay: ' + name)
    return {'patches': patch_records(), 'sourceFiles': {
        'dlls/wined3d/' + name: sha((source / 'dlls/wined3d' / name).read_bytes())
        for name in SOURCE_FILES}}


def validate_module(data, arch):
    if len(data) < 64 or data[:2] != b'MZ':
        raise ValueError(f'{arch}: WineD3D is not a PE module')
    offset = struct.unpack_from('<I', data, 60)[0]
    if (offset + 24 > len(data) or data[offset:offset+4] != b'PE\0\0'
            or struct.unpack_from('<H', data, offset+4)[0] != MODULES[arch]):
        raise ValueError(f'{arch}: WineD3D PE architecture mismatch')
    for marker in (b'Framebuffer sRGB write control is unavailable',
                   b'glEnable(GL_ALPHA_TEST)', b'glDisable(GL_ALPHA_TEST)',
                   b'Failed to create command stream event.'):
        if marker not in data:
            raise ValueError(f'{arch}: missing WineD3D fix: {marker.decode()}')
    if arch == 'i386-windows' and b'dynamic_buffer_sysmem' not in data:
        raise ValueError(f'{arch}: missing opt-in dynamic buffer policy')
    if b'NtAlertThreadByThreadId' in data or b'NtWaitForAlertByThreadId' in data:
        raise ValueError(f'{arch}: obsolete thread-ID command queue wake backend')


def validate_runtime(record, read):
    if record.get('version') != 1 or record.get('patches') != patch_records():
        raise ValueError('WineD3D graphics overlay inventory/hash mismatch')
    sources = record.get('sourceFiles', {})
    if (set(sources) != {'dlls/wined3d/' + name for name in SOURCE_FILES}
            or not all(re.fullmatch('[0-9a-f]{64}', value) for value in sources.values())):
        raise ValueError('WineD3D effective source identities are incomplete')
    files = record.get('files', [])
    if len(files) != len(MODULES) or {item['path'] for item in files} != {
            f'bin/{arch}/wined3d.dll' for arch in MODULES}:
        raise ValueError('WineD3D graphics architecture inventory mismatch')
    for item in files:
        data = read(item['path'])
        if len(data) != item['bytes'] or sha(data) != item['sha256']:
            raise ValueError('WineD3D graphics DLL identity mismatch: ' + item['path'])
        validate_module(data, item['path'].split('/')[1])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', type=Path, required=True)
    parser.add_argument('--wine-source', type=Path)
    parser.add_argument('--manifest', type=Path)
    args = parser.parse_args()
    marker = args.directory / 'wined3d-graphics.json'
    if args.wine_source:
        record = {'version': 1, **source_contract(args.wine_source), 'files': []}
        for arch in MODULES:
            path = f'bin/{arch}/wined3d.dll'
            data = (args.directory / path).read_bytes()
            record['files'].append({'path': path, 'bytes': len(data), 'sha256': sha(data)})
    elif args.manifest:
        record = json.loads(marker.read_text())
    else:
        parser.error('--wine-source or --manifest is required')
    validate_runtime(record, lambda path: (args.directory / path).read_bytes())
    if args.wine_source:
        marker.write_text(json.dumps(record, indent=2) + '\n')
    if args.manifest:
        manifest = json.loads(args.manifest.read_text())
        manifest['wineD3DGraphics'] = record
        manifest['commandQueueWake'] = {
            'version': 1, 'backend': 'auto_reset_event',
            'patchSha256': record['patches'][PATCHES[-1]], 'files': record['files']}
        args.manifest.write_text(json.dumps(manifest, indent=2) + '\n')
    print('WineD3D effective overlays and paired graphics/queue binaries verified')


if __name__ == '__main__':
    main()
