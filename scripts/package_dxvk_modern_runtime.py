#!/usr/bin/env python3
"""Replace only the six reviewed Modern DXVK DLLs and their runtime metadata."""
import argparse
import copy
import hashlib
import io
import json
import re
from pathlib import Path
from zipfile import ZipFile

from dxvk_runtime_contract import ROOT, load_modern_lock, source_digest, validate_modern_runtime


def package(rawfile, profile):
    lock_path = ROOT / 'docs/graphics/graphics-stack.lock.yaml'
    pinned = load_modern_lock()
    if source_digest(ROOT / 'thirdparty/dxvk-modern/src') != pinned['sourceSha256']:
        raise ValueError('reviewed Modern DXVK source fingerprint does not match')
    replacement = {}
    for name, expected in pinned['files'].items():
        data = (profile / name).read_bytes()
        if hashlib.sha256(data).hexdigest() != expected:
            raise ValueError('unreviewed Modern DLL: ' + name)
        replacement['dxvk/modern-2.6/' + name] = data
    payload_path = rawfile / 'wine-data.zip'
    output = io.BytesIO()
    with ZipFile(payload_path) as original:
        manifest = json.loads(original.read('dxvk/manifest.json'))
        modern = manifest['runtimes']['modern-2.6']
        modern['commit'] = pinned['commit']
        for name, digest in pinned['files'].items():
            architecture, filename = Path(name).parts
            modern.setdefault(architecture, {})[filename] = digest
        replacement['dxvk/manifest.json'] = (json.dumps(manifest, indent=2) + '\n').encode()
        with ZipFile(output, 'w') as result:
            for member in original.infolist():
                result.writestr(copy.copy(member), replacement.get(member.filename, original.read(member.filename)))
    payload = output.getvalue()
    with ZipFile(io.BytesIO(payload)) as result:
        validate_modern_runtime(result)
    manifest_path = rawfile / 'wine-runtime-manifest.json'
    outer = json.loads(manifest_path.read_text())
    digest = hashlib.sha256(payload).hexdigest()
    lock_digest = hashlib.sha256(lock_path.read_bytes()).hexdigest()
    outer['payloadSha256'] = digest
    if 'graphicsMigration' in outer:
        outer['graphicsMigration']['lockSha256'] = lock_digest
    version_path = rawfile / 'wine_runtime.version'
    version = re.sub(r'content=[^;\s]+', 'content=' + digest[:16], version_path.read_text())
    version = re.sub(r'graphics-migration=[^;\s]+', 'graphics-migration=' + lock_digest[:16], version)
    for target, data in [(payload_path, payload), (manifest_path, (json.dumps(outer, indent=2) + '\n').encode()),
                         (version_path, version.encode())]:
        temporary = target.with_name(target.name + '.dxvk.tmp')
        temporary.write_bytes(data)
        temporary.replace(target)
    print('Modern DXVK package PASS: six pinned DLLs, manifest, extraction version and graphics lock')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rawfile', type=Path, required=True)
    parser.add_argument('--profile-dir', type=Path, required=True)
    args = parser.parse_args()
    package(args.rawfile, args.profile_dir)
