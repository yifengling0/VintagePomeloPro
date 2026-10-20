#!/usr/bin/env python3
"""Make a local import bundle. Valve bytes remain outside the HAP."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import zipfile


def file_hash(path):
    result = hashlib.sha256()
    with path.open('rb') as source:
        for data in iter(lambda: source.read(1048576), b''):
            result.update(data)
    return result.hexdigest()


def build(plan_path, downloads, output, rawfile_dir):
    plan = json.loads(plan_path.read_text(encoding='utf-8'))
    sources = []
    for archive in plan['archives']:
        if archive['url'].startswith('rawfile://'):
            continue
        source = downloads / archive['url'].rsplit('/', 1)[1]
        if source.stat().st_size != archive['bytes'] or file_hash(source) != archive['sha256']:
            raise ValueError('Official ZIP checksum mismatch: ' + archive['id'])
        sources.append(('archives/' + archive['sha256'] + '.zip', source))
    info = json.dumps({'schemaVersion': 1, 'id': plan['id'], 'clientBuild': plan['clientBuild']}, sort_keys=True).encode()
    temporary = output.with_suffix('.tmp')
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(temporary, 'w', compression=zipfile.ZIP_STORED) as archive:
        for name, source in [('bundle-info.json', None), *sorted(sources)]:
            member = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0))
            member.external_attr = 0o100644 << 16
            with archive.open(member, 'w') as target:
                if source:
                    with source.open('rb') as data:
                        shutil.copyfileobj(data, target, 1048576)
                else:
                    target.write(info)
    # ArkTS's shared SHA helper deliberately bounds a single file to 512 MiB.
    if temporary.stat().st_size > 536870912:
        temporary.unlink()
        raise ValueError('Offline package exceeds this importer version; split support is required')
    temporary.replace(output)
    metadata = {'schemaVersion': 1, 'id': plan['id'], 'bytes': output.stat().st_size,
                'sha256': file_hash(output), 'fileName': output.name}
    rawfile_dir.mkdir(parents=True, exist_ok=True)
    (rawfile_dir / 'offline-package.json').write_text(json.dumps(metadata, sort_keys=True) + '\n', encoding='utf-8')
    return metadata


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for key in ('plan', 'downloads', 'output', 'rawfile-dir'):
        parser.add_argument('--' + key, type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(build(args.plan, args.downloads, args.output, args.rawfile_dir)))
