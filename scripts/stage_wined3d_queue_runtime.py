#!/usr/bin/env python3
"""Stage verified OHOS WineD3D event-queue builds into the existing runtime."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import zipfile

from wined3d_runtime_contract import MODULES, source_contract, validate_module, validate_runtime
from stamp_wine_builtin_identity import stamp_archive

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(4 * 1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--modules-dir', required=True, type=Path)
    parser.add_argument('--wine-source', required=True, type=Path)
    parser.add_argument('--expected-payload-sha256', required=True)
    args = parser.parse_args()
    raw = ROOT / 'entry/src/main/resources/rawfile'
    archive = raw / 'wine-data.zip'
    manifest_path = raw / 'wine-runtime-manifest.json'
    version_path = raw / 'wine_runtime.version'
    try:
        graphics = {'version': 1, **source_contract(args.wine_source), 'files': []}
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        parser.error(str(error))
    replacements, files = {}, []
    for arch in MODULES:
        path = args.modules_dir / arch / 'wined3d.dll'
        data = path.read_bytes()
        try:
            validate_module(data, arch)
        except ValueError as error:
            parser.error(str(error))
        member = f'bin/{arch}/wined3d.dll'
        replacements[member] = data
        files.append({'path': member, 'bytes': len(data), 'sha256': sha(path)})
    graphics['files'] = files
    graphics_data = (json.dumps(graphics, indent=2) + '\n').encode()
    graphics_member = 'wined3d-graphics.json'
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
        replacements['share/wine/wine.inf'] = stamp_archive(source, replacements)
        installed = (all(source.read(name) == data for name, data in replacements.items())
                     and manifest.get('commandQueueWake') == marker
                     and manifest.get('wineD3DGraphics') == graphics
                     and graphics_member in source.namelist()
                     and source.read(graphics_member) == graphics_data)
    if current != args.expected_payload_sha256 and not installed:
        parser.error('Refusing to replace a different Wine runtime')
    temporary = archive.with_suffix('.queue-pending.zip')
    try:
        if not installed:
            with zipfile.ZipFile(archive) as source, zipfile.ZipFile(temporary, 'w') as target:
                for member in source.infolist():
                    if member.filename == graphics_member:
                        continue
                    elif member.filename in replacements:
                        target.writestr(member, replacements[member.filename])
                    else:
                        with source.open(member) as reader, target.open(member, 'w') as writer:
                            shutil.copyfileobj(reader, writer, 4 * 1024 * 1024)
                target.writestr(graphics_member, graphics_data, compress_type=zipfile.ZIP_DEFLATED)
            with zipfile.ZipFile(temporary) as result:
                validate_runtime(json.loads(result.read(graphics_member)), result.read)
            current = sha(temporary)
            temporary.replace(archive)
        manifest['payloadSha256'] = current
        manifest['commandQueueWake'] = marker
        manifest['wineD3DGraphics'] = graphics
        text = re.sub(r'^content=[0-9a-f]+', 'content=' + current[:16], version_path.read_text().strip())
        text = re.sub(r';command-queue=[0-9a-f]+', '', text)
        text += ';command-queue=' + marker['patchSha256'][:16] + '\n'
        text = re.sub(r';wined3d-graphics=[0-9a-f]+', '', text).strip()
        text += ';wined3d-graphics=' + hashlib.sha256(graphics_data).hexdigest()[:16] + '\n'
        for path, value in [(manifest_path, json.dumps(manifest, indent=2) + '\n'), (version_path, text)]:
            pending = path.with_suffix(path.suffix + '.queue-pending')
            pending.write_text(value)
            pending.replace(path)
        print(json.dumps({'payloadSha256': current, 'commandQueueWake': marker}))
    finally:
        temporary.unlink(missing_ok=True)


if __name__ == '__main__':
    main()
