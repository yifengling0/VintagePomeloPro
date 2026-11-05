#!/usr/bin/env python3
"""Replace only the verified Box DLL in an existing Wine runtime archive."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import zipfile
from apply_box64_wow64_patches import ROOT, apply, validate_module
from stamp_wine_builtin_identity import stamp_archive


def sha(path):
    result = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(4*1024*1024), b''):
            result.update(block)
    return result.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--module', type=Path, required=True)
    parser.add_argument('--expected-payload-sha256', required=True)
    args = parser.parse_args()
    apply(verify_only=True)
    data = args.module.read_bytes()
    lock = validate_module(data)
    raw = ROOT / 'entry/src/main/resources/rawfile'
    archive = raw / 'wine-data.zip'
    manifest_path = raw / 'wine-runtime-manifest.json'
    version_path = raw / 'wine_runtime.version'
    manifest = json.loads(manifest_path.read_text())
    current = sha(archive)
    if current != args.expected_payload_sha256 or manifest['payloadSha256'] != current:
        parser.error('Wine payload changed; refusing to overwrite a different runtime')
    member = 'bin/aarch64-windows/wowbox64.dll'
    marker = {'version': 1, 'runtimeMarker': lock['runtimeMarker'],
              'sourceCommit': lock['sourceCommit'], 'patches': lock['patches'],
              'moduleSha256': hashlib.sha256(data).hexdigest()}
    pending = archive.with_suffix('.box-pending.zip')
    try:
        with zipfile.ZipFile(archive) as source:
            names = source.namelist()
            if len(names) != len(set(names)) or member not in names:
                parser.error('Wine archive has duplicate entries or lacks Box DLL')
            replacements = {member: data}
            replacements['share/wine/wine.inf'] = stamp_archive(source, replacements)
            with zipfile.ZipFile(pending, 'w') as target:
                for info in source.infolist():
                    if info.filename in replacements:
                        target.writestr(info, replacements[info.filename])
                    else:
                        with source.open(info) as reader, target.open(info, 'w') as writer:
                            shutil.copyfileobj(reader, writer, 4*1024*1024)
        with zipfile.ZipFile(pending) as result:
            validate_module(result.read(member))
        current = sha(pending)
        pending.replace(archive)
        manifest['payloadSha256'] = current
        manifest['box64Wow64'] = marker
        manifest_path.write_text(json.dumps(manifest, indent=2) + '\n')
        version = re.sub(r'^content=[0-9a-f]+', 'content=' + current[:16], version_path.read_text().strip())
        version = re.sub(r';box-wow64=[0-9a-f]+', '', version)
        version_path.write_text(version + ';box-wow64=' + marker['moduleSha256'][:16] + '\n')
        print(json.dumps({'payloadSha256': current, 'box64Wow64': marker}))
    finally:
        pending.unlink(missing_ok=True)


if __name__ == '__main__':
    main()
