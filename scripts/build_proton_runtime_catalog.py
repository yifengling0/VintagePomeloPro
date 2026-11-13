#!/usr/bin/env python3
"""Publishable upstream-only Proton inventory; adapter binaries are independent.

The default catalog reuses the already published, checksum-pinned component ZIP.
Only its package/ members are activated. --split-output can produce a smaller
upstream-only ZIP without rebuilding or modifying any GE Wine/FEX files.
"""
import argparse
import hashlib
import json
from pathlib import Path
import zipfile

ROOT = Path(__file__).resolve().parents[1]
RAW = ROOT / 'entry/src/main/resources/rawfile/linux-proton'
CATALOG = ROOT / 'docs/linux-steam/proton-runtimes/catalog.json'
ABI = 'hmosdeck-ge-wine11-arm64ec-v1'


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest() if hasattr(hashlib, 'file_digest') else _sha(stream)


def _sha(stream):
    value = hashlib.sha256()
    for chunk in iter(lambda: stream.read(1024 * 1024), b''): value.update(chunk)
    return value.hexdigest()


def release(root=ROOT):
    raw = root / 'entry/src/main/resources/rawfile/linux-proton'
    manifest = json.loads((raw / 'manifest.json').read_text())
    lock = json.loads((root / 'docs/linux-steam/build-components.lock.json').read_text())
    archive = next(row for row in lock['components'] if row['path'] == 'linux-proton/gnu-proton.zip')
    if archive['bytes'] != manifest['bytes'] or archive['sha256'] != manifest['sha256']:
        raise ValueError('Bundled component and dependency lock differ')
    files = [dict(row) for row in manifest['files'] if row['path'].startswith('package/')]
    upstream = json.loads((root / 'docs/linux-steam/gnu-proton-probe.lock.json').read_text())
    for source, alias in upstream['aliases'].items():
        target = 'package/' + str(Path(source).parent / alias)
        for row in files:
            if row['path'] == target: row['sourcePath'] = 'package/' + source
    core = dict(name=manifest['distribution']['tag'] + ' ARM64', architecture='aarch64', adapterAbi=ABI,
                prefixAbi='wine11-arm64ec-v1', files=files)
    identity = hashlib.sha256(json.dumps(core, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
    return dict(id=identity, **core, installedBytes=sum(row['bytes'] for row in files),
                archive=dict(url=archive['url'], bytes=archive['bytes'], sha256=archive['sha256'],
                             unpackedBytes=manifest['installedBytes']))


def split_archive(source, output, descriptor):
    if source.stat().st_size != descriptor['archive']['bytes'] or sha(source) != descriptor['archive']['sha256']:
        raise ValueError('Source component ZIP differs from the pinned upstream inventory')
    files = descriptor['files']
    expected = {row['path']: row for row in files}
    if output.exists():
        with zipfile.ZipFile(output) as archive:
            if set(archive.namelist()) == set(expected) and all(
                archive.getinfo(name).file_size == row['bytes'] and
                hashlib.sha256(archive.read(name)).hexdigest() == row['sha256'] for name, row in expected.items()):
                return dict(bytes=output.stat().st_size, sha256=sha(output), unpackedBytes=descriptor['installedBytes'])
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_suffix('.part')
    with zipfile.ZipFile(source) as original, zipfile.ZipFile(temporary, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        for name, row in expected.items():
            data = original.read(name)
            if len(data) != row['bytes'] or hashlib.sha256(data).hexdigest() != row['sha256']:
                raise ValueError('Changed upstream file: ' + name)
            info = zipfile.ZipInfo(name, (2026, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = (0o100000 | row['mode']) << 16
            archive.writestr(info, data)
    temporary.replace(output)
    return dict(bytes=output.stat().st_size, sha256=sha(output), unpackedBytes=descriptor['installedBytes'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--write', action='store_true', help='Update both source and bundled catalogs')
    parser.add_argument('--split-output', type=Path, help='Optional reusable upstream-only release ZIP')
    parser.add_argument('--archive-url', help='Published HTTPS URL for the upstream-only ZIP')
    args = parser.parse_args()
    descriptor = release()
    if bool(args.split_output) != bool(args.archive_url):
        parser.error('--split-output and --archive-url must be provided together')
    if args.split_output:
        descriptor['archive'] = dict(url=args.archive_url, **split_archive(RAW / 'gnu-proton.zip', args.split_output, descriptor))
    catalog = json.loads(CATALOG.read_text()) if CATALOG.exists() else dict(schemaVersion=1, revision=1, releases=[])
    existing = next((r for r in catalog['releases'] if r['id'] == descriptor['id']), None)
    # Replacing only the transfer archive does not alter executable identity.
    if existing and not args.archive_url: descriptor['archive'] = existing['archive']
    if args.write:
        if existing != descriptor:
            had_releases = bool(catalog['releases'])
            catalog['releases'] = [descriptor] + [r for r in catalog['releases'] if r['id'] != descriptor['id']]
            if had_releases: catalog['revision'] += 1
        data = json.dumps(catalog, indent=2) + '\n'
        CATALOG.parent.mkdir(parents=True, exist_ok=True)
        CATALOG.write_text(data)
        (RAW / 'runtime-catalog.json').write_text(data)
    else:
        if existing != descriptor or (RAW / 'runtime-catalog.json').read_bytes() != CATALOG.read_bytes():
            raise ValueError('Upstream-only runtime catalog is stale; run --write')
    print(json.dumps(dict(result='PASS', id=descriptor['id'], files=len(descriptor['files']),
                          upstreamBytes=descriptor['installedBytes'], archive=descriptor['archive'])))


if __name__ == '__main__': main()
