#!/usr/bin/env python3
"""Stage verified OHOS WineD3D event-queue builds into the existing runtime."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import zipfile

ROOT = Path(__file__).resolve().parents[1]
MODULES = {'i386-windows': 0x14c, 'aarch64-windows': 0xaa64}


def sha(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(4 * 1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--modules-dir', required=True, type=Path)
    parser.add_argument('--expected-payload-sha256', required=True)
    args = parser.parse_args()
    raw = ROOT / 'entry/src/main/resources/rawfile'
    archive = raw / 'wine-data.zip'
    manifest_path = raw / 'wine-runtime-manifest.json'
    version_path = raw / 'wine_runtime.version'
    replacements, files = {}, []
    for arch, machine in MODULES.items():
        path = args.modules_dir / arch / 'wined3d.dll'
        data = path.read_bytes()
        if len(data) < 64 or data[:2] != b'MZ':
            parser.error(f'{arch}: not a PE module')
        offset = struct.unpack_from('<I', data, 60)[0]
        if (data[offset:offset+4] != b'PE\0\0' or
                struct.unpack_from('<H', data, offset+4)[0] != machine or
                b'NtAlertThreadByThreadId' in data or b'NtWaitForAlertByThreadId' in data or
                b'Failed to create command stream event.' not in data):
            parser.error(f'{arch}: expected the event-only WineD3D build')
        member = f'bin/{arch}/wined3d.dll'
        replacements[member] = data
        files.append({'path': member, 'bytes': len(data), 'sha256': sha(path)})
    marker = {'version': 1, 'backend': 'auto_reset_event',
              'patchSha256': sha(ROOT / 'patches/wine/0068-wined3d-ohos-command-event-wake.patch'),
              'files': files}
    manifest = json.loads(manifest_path.read_text())
    current = sha(archive)
    if manifest['payloadSha256'] != current:
        parser.error('Wine manifest does not describe the current payload')
    with zipfile.ZipFile(archive) as source:
        if not replacements.keys() <= set(source.namelist()):
            parser.error('Wine runtime is missing an architecture; refusing a partial update')
        installed = (all(source.read(name) == data for name, data in replacements.items())
                     and manifest.get('commandQueueWake') == marker)
    if current != args.expected_payload_sha256 and not installed:
        parser.error('Refusing to replace a different Wine runtime')
    temporary = archive.with_suffix('.queue-pending.zip')
    try:
        if not installed:
            with zipfile.ZipFile(archive) as source, zipfile.ZipFile(temporary, 'w') as target:
                for member in source.infolist():
                    if member.filename in replacements:
                        target.writestr(member, replacements[member.filename])
                    else:
                        with source.open(member) as reader, target.open(member, 'w') as writer:
                            shutil.copyfileobj(reader, writer, 4 * 1024 * 1024)
            with zipfile.ZipFile(temporary) as result:
                for record in files:
                    assert hashlib.sha256(result.read(record['path'])).hexdigest() == record['sha256']
            current = sha(temporary)
            temporary.replace(archive)
        manifest['payloadSha256'] = current
        manifest['commandQueueWake'] = marker
        text = re.sub(r'^content=[0-9a-f]+', 'content=' + current[:16], version_path.read_text().strip())
        text = re.sub(r';command-queue=[0-9a-f]+', '', text)
        text += ';command-queue=' + marker['patchSha256'][:16] + '\n'
        for path, value in [(manifest_path, json.dumps(manifest, indent=2) + '\n'), (version_path, text)]:
            pending = path.with_suffix(path.suffix + '.queue-pending')
            pending.write_text(value)
            pending.replace(path)
        print(json.dumps({'payloadSha256': current, 'commandQueueWake': marker}))
    finally:
        temporary.unlink(missing_ok=True)


if __name__ == '__main__':
    main()
