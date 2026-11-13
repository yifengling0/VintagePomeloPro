#!/usr/bin/env python3
"""Stage the reviewed ARM64 Wineboot with its source patch and runtime identity."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import zipfile

ROOT = Path(__file__).resolve().parents[1]

def sha(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(4*1024*1024), b''):
            digest.update(block)
    return digest.hexdigest()

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--wineboot', required=True, type=Path)
    parser.add_argument('--expected-payload-sha256', required=True)
    args = parser.parse_args()
    raw = ROOT/'entry/src/main/resources/rawfile'
    archive = raw/'wine-data.zip'
    manifest_path = raw/'wine-runtime-manifest.json'
    version_path = raw/'wine_runtime.version'
    module = args.wineboot.read_bytes()
    offset = struct.unpack_from('<I', module, 60)[0]
    if (module[:2] != b'MZ' or module[offset:offset+4] != b'PE\0\0' or
            struct.unpack_from('<H', module, offset+4)[0] not in {0xaa64, 0xa64e} or
            b'--winehua-managed-init' not in module or b'wineboot-init-ok' not in module):
        parser.error('Expected the managed-initialization ARM64 Wine builtin')
    marker = {'version': 1, 'path': 'bin/wineboot.exe', 'sha256': sha(args.wineboot),
              'patchSha256': sha(ROOT/'patches/wine/0067-wineboot-single-managed-initialization.patch')}
    manifest = json.loads(manifest_path.read_text())
    current = sha(archive)
    if manifest['payloadSha256'] != current:
        parser.error('Wine manifest does not describe current payload')
    with zipfile.ZipFile(archive) as source:
        installed = (source.read(marker['path']) == module and manifest.get('managedInitialization') == marker)
    if current != args.expected_payload_sha256 and not installed:
        parser.error('Refusing to replace a different Wine runtime')
    temporary = archive.with_suffix('.wineboot-pending.zip')
    try:
        if not installed:
            with zipfile.ZipFile(archive) as source, zipfile.ZipFile(temporary, 'w') as target:
                for member in source.infolist():
                    if member.filename == marker['path']:
                        target.writestr(member, module)
                    else:
                        with source.open(member) as reader, target.open(member, 'w') as writer:
                            shutil.copyfileobj(reader, writer, 4*1024*1024)
            with zipfile.ZipFile(temporary) as result:
                assert hashlib.sha256(result.read(marker['path'])).hexdigest() == marker['sha256']
            current = sha(temporary)
            temporary.replace(archive)
        manifest['payloadSha256'] = current
        manifest['managedInitialization'] = marker
        text = re.sub(r'^content=[0-9a-f]+', 'content='+current[:16], version_path.read_text().strip())
        text = re.sub(r';managed-init=[0-9a-f]+', '', text)
        text += ';managed-init='+marker['sha256'][:16]+'\n'
        for path, value in [(manifest_path, json.dumps(manifest, indent=2)+'\n'), (version_path, text)]:
            pending = path.with_suffix(path.suffix+'.wineboot-pending')
            pending.write_text(value)
            pending.replace(path)
        print(json.dumps({'payloadSha256': current, 'managedInitialization': marker}))
    finally:
        temporary.unlink(missing_ok=True)

if __name__ == '__main__':
    main()
