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


def compatible_bundle(path, expected):
    """Validate an earlier bundle without extracting any files or trusting its ID."""
    try:
        with zipfile.ZipFile(path) as bundle:
            members = bundle.infolist()
            if len(members) != len(expected) + 1 or {m.filename for m in members} != {'bundle-info.json', *expected}:
                return False
            if bundle.getinfo('bundle-info.json').file_size > 4096:
                return False
            for name, (size, checksum) in expected.items():
                member = bundle.getinfo(name)
                if member.file_size != size or member.external_attr >> 16 != 0o100644:
                    return False
                with bundle.open(member) as data:
                    digest = hashlib.sha256()
                    for block in iter(lambda: data.read(1048576), b''):
                        digest.update(block)
                    if digest.hexdigest() != checksum:
                        return False
        return True
    except (OSError, ValueError, zipfile.BadZipFile, RuntimeError):
        return False


def build(plan_path, downloads, output, rawfile_dir, compatible_packages=()):
    plan = json.loads(plan_path.read_text(encoding='utf-8'))
    sources = []
    for archive in plan['archives']:
        if archive['url'].startswith('rawfile://'):
            continue
        source = downloads / archive['url'].rsplit('/', 1)[1]
        if source.stat().st_size != archive['bytes'] or file_hash(source) != archive['sha256']:
            raise ValueError('Official ZIP checksum mismatch: ' + archive['id'])
        sources.append(('archives/' + archive['sha256'] + '.zip', source))
    expected = {name: (source.stat().st_size, file_hash(source)) for name, source in sources}
    archive_set = hashlib.sha256(json.dumps(expected, sort_keys=True).encode()).hexdigest()
    # GNU/host updates must not rebuild an identical 330 MiB official bundle.
    # Legacy bundle-info release IDs are harmless: only current official ZIPs
    # are accepted, after checking every byte against the current plan.
    if output.is_file() and compatible_bundle(output, expected):
        return write_metadata(plan, output, rawfile_dir, archive_set, expected, compatible_packages)
    info = json.dumps({'schemaVersion': 1, 'id': archive_set, 'clientBuild': plan['clientBuild']}, sort_keys=True).encode()
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
    return write_metadata(plan, output, rawfile_dir, archive_set, expected, compatible_packages)


def write_metadata(plan, output, rawfile_dir, archive_set, expected, compatible_packages):
    previous_path = rawfile_dir / 'offline-package.json'
    previous = json.loads(previous_path.read_text(encoding='utf-8')) if previous_path.is_file() else {}
    identities = {}
    # Previously verified publications remain accepted only while the official
    # archive set is identical. Unknown or corrupt archives are never unpacked.
    if previous.get('archiveSetId') == archive_set:
        for item in [previous, *previous.get('compatiblePackages', [])]:
            identities[item['sha256']] = {'bytes': item['bytes'], 'sha256': item['sha256']}
    for path in compatible_packages:
        if not compatible_bundle(path, expected):
            raise ValueError('Previous offline bundle is incompatible or corrupt: ' + str(path))
        checksum = file_hash(path)
        identities[checksum] = {'bytes': path.stat().st_size, 'sha256': checksum}
    checksum = file_hash(output)
    identities.pop(checksum, None)
    if len(identities) > 16:
        raise ValueError('Too many compatible offline package identities')
    metadata = {'schemaVersion': 1, 'id': plan['id'], 'archiveSetId': archive_set,
                'bytes': output.stat().st_size, 'sha256': checksum, 'fileName': output.name,
                'compatiblePackages': sorted(identities.values(), key=lambda item: item['sha256'])}
    rawfile_dir.mkdir(parents=True, exist_ok=True)
    (rawfile_dir / 'offline-package.json').write_text(json.dumps(metadata, sort_keys=True) + '\n', encoding='utf-8')
    return metadata


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for key in ('plan', 'downloads', 'output', 'rawfile-dir'):
        parser.add_argument('--' + key, type=Path, required=True)
    parser.add_argument('--compatible-package', type=Path, action='append', default=[],
                        help='Validate and accept an earlier published bundle with the same official ZIPs')
    args = parser.parse_args()
    print(json.dumps(build(args.plan, args.downloads, args.output, args.rawfile_dir, args.compatible_package)))
