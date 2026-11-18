#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Prepare pinned Gamescope sources without installation.

This is source preparation only. It does not qualify Vulkan capabilities or
fetch undeclared Meson fallbacks. All source notices remain in the archives.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import sys
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]
from locked_download import fetch, open_owned, sha256

BASE = ROOT/'build/gamescope'


def relative(value):
    path = PurePosixPath(value)
    if (not value or '\\' in value or ':' in value or path.is_absolute()
            or any(p in ('', '.', '..') for p in value.split('/'))):
        raise ValueError('Invalid source path: '+repr(value))
    return path


def extract(archive, record, destination, budget):
    prefix = record['archiveRoot']
    relative(prefix)
    if '/' in prefix:
        raise ValueError('Archive root must be one directory')
    with open_owned(archive, os.O_RDONLY) as stream, tarfile.open(fileobj=stream, mode='r:gz') as package:
        for member in package:
            budget['entries'] += 1
            name = member.name.rstrip('/')
            relative(name)
            if name == prefix and member.isdir():
                continue
            if not name.startswith(prefix+'/') or not (member.isdir() or member.isfile()):
                raise ValueError('Unsupported source member: '+name)
            path = destination/str(relative(name[len(prefix)+1:]))
            if budget['entries'] > budget['maxEntries']:
                raise ValueError('Source entry budget exceeded')
            if member.isdir():
                path.mkdir(parents=True, exist_ok=True)
                continue
            budget['bytes'] += member.size
            if member.size < 0 or budget['bytes'] > budget['maxExpandedBytes']:
                raise ValueError('Expanded source budget exceeded')
            path.parent.mkdir(parents=True, exist_ok=True)
            # No links, special files or overlays between archives. Only the
            # explicitly pinned upstream Meson overlay is applied afterwards.
            with package.extractfile(member) as source, path.open('xb') as output:
                shutil.copyfileobj(source, output, 1 << 20)
            if path.stat().st_size != member.size:
                raise ValueError('Truncated source member')
            path.chmod(0o755 if member.mode & 0o111 else 0o644)


def inventory(directory):
    result = {}
    for path in sorted(directory.rglob('*')):
        if path.is_symlink():
            raise ValueError('Unexpected prepared source link')
        if path.is_file() and path.name != '.hsl-source-receipt.json':
            result[path.relative_to(directory).as_posix()] = {
                'bytes': path.stat().st_size, 'sha256': sha256(path)}
    return result


def prepare(lock, cache, output, download=False):
    if lock['schemaVersion'] != 1 or lock['runtimeEligible'] is not False:
        raise ValueError('Unsupported Gamescope source lock')
    inputs = {'lock': lock, 'preparerSha256': sha256(Path(__file__))}
    identity = hashlib.sha256(json.dumps(inputs, sort_keys=True).encode()).hexdigest()
    cache.mkdir(parents=True, exist_ok=True)
    output.mkdir(parents=True, exist_ok=True)
    archives = []
    paths = set()
    for record in lock['sources']:
        if record['path'] != '.':
            relative(record['path'])
        if record['path'] in paths or not re.fullmatch('[0-9a-f]{40}', record['revision']):
            raise ValueError('Duplicate source destination or invalid revision')
        paths.add(record['path'])
        if len(relative(record['archive']).parts) != 1:
            raise ValueError('Source archive must be a basename')
        archive = cache/record['archive']
        if download:
            fetch(record['url'], record['bytes'], record['sha256'], archive,
                  origins=lock['origins'], extra_bytes=lock['maxExpandedBytes'])
        if archive.stat().st_size != record['bytes'] or sha256(archive) != record['sha256']:
            raise ValueError('Source archive identity mismatch: '+record['archive'])
        archives.append((archive, record))
    target = output/identity
    receipt = target/'.hsl-source-receipt.json'
    if target.exists():
        saved = json.loads(receipt.read_text(encoding='utf8'))
        if saved['inputs'] != inputs or saved['files'] != inventory(target):
            raise ValueError('Prepared source modified; existing tree preserved')
        return target
    with tempfile.TemporaryDirectory(prefix='prepare-', dir=output) as temporary:
        stage = Path(temporary)
        budget = dict(entries=0, bytes=0, maxEntries=lock['maxEntries'],
                      maxExpandedBytes=lock['maxExpandedBytes'])
        for archive, record in archives:
            destination = stage if record['path'] == '.' else stage/record['path']
            destination.mkdir(parents=True, exist_ok=True)
            extract(archive, record, destination, budget)
        for source, destination in lock['mesonOverlays'].items():
            src, dst = stage/str(relative(source)), stage/str(relative(destination))
            if not src.is_dir() or not dst.is_dir():
                raise ValueError('Missing locked Meson overlay')
            shutil.copytree(src, dst, dirs_exist_ok=True)
        (stage/receipt.name).write_text(json.dumps({'inputs': inputs, 'files': inventory(stage),
            'entries': budget['entries'], 'expandedBytes': budget['bytes'],
            'runtimeEligible': False}, indent=2)+'\n', encoding='utf8')
        if target.exists():
            raise ValueError('Concurrent source preparation; existing tree preserved')
        stage.rename(target)
    return target


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--download', action='store_true', help='Fetch missing pinned HTTPS archives')
    parser.add_argument('--cache', type=Path, default=BASE/'cache')
    parser.add_argument('--output', type=Path, default=BASE/'pristine')
    args = parser.parse_args()
    lock = json.loads((ROOT/'components/gamescope-source.lock.json').read_text(encoding='utf8'))
    print(prepare(lock, args.cache, args.output, args.download))


if __name__ == '__main__':
    main()
