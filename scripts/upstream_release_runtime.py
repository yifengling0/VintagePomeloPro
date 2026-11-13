#!/usr/bin/env python3
"""Enforce the branch's selected, tested upstream Wine/FEX bytes at packaging."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
from zipfile import ZipFile

ROOT = Path(__file__).resolve().parents[1]
LOCK = ROOT / 'docs/upstream-sync/release-runtime.lock.json'
RAW = 'resources/rawfile/'


def digest(stream):
    value = hashlib.sha256()
    for chunk in iter(lambda: stream.read(1024 * 1024), b''):
        value.update(chunk)
    return value.hexdigest()


def file_sha(path):
    with path.open('rb') as stream:
        return digest(stream)


def load_lock():
    lock = json.loads(LOCK.read_text())
    if lock['schemaVersion'] != 1:
        raise ValueError('Unknown selected upstream runtime schema')
    for component in ('wine', 'fex'):
        item = lock[component]
        path = ROOT / item['lockFile']
        if file_sha(path) != item['lockSha256']:
            raise ValueError('Selected upstream source lock changed: ' + component)
        source = json.loads(path.read_text())
        if source['adaptedCommit'] != item['adaptedCommit']:
            raise ValueError('Selected upstream source identity changed: ' + component)
        if 'packageLockFile' in item:
            original = ROOT / item['packageLockFile']
            if file_sha(original) != item['packageLockSha256']:
                raise ValueError('Original packaged source lock changed: ' + component)
            previous = json.loads(original.read_text())
            if any(previous[k] != source[k] for k in ('adaptedCommit', 'adaptedTree')):
                raise ValueError('Reverified source differs from packaged source: ' + component)
    return lock


def verify(open_member, names, lock):
    names = set(names)
    for name, expected in lock['members'].items():
        if name not in names:
            raise ValueError('Selected upstream runtime member missing: ' + name)
        with open_member(name) as stream:
            if digest(stream) != expected['sha256']:
                raise ValueError('Selected upstream runtime bytes differ: ' + name)
    with open_member(RAW + 'wine-runtime-manifest.json') as stream:
        manifest = json.load(stream)
    for component in ('wine', 'fex'):
        actual = manifest.get('componentOverrides', {}).get(component, {})
        if actual.get('adaptedCommit') != lock[component]['adaptedCommit']:
            raise ValueError('Old or mixed upstream runtime selected: ' + component)
        expected_lock = lock[component].get('packageLockSha256', lock[component]['lockSha256'])
        if actual.get('lockSha256') != expected_lock:
            raise ValueError('Upstream runtime source lock mismatch: ' + component)
    return dict(wine=lock['wine']['adaptedCommit'], fex=lock['fex']['adaptedCommit'],
                verifiedMembers=len(lock['members']))


def verify_hap(path, lock=None):
    with ZipFile(path) as archive:
        if len(archive.namelist()) != len(set(archive.namelist())):
            raise ValueError('Duplicate upstream HAP members')
        return verify(archive.open, archive.namelist(), lock or load_lock())


def staged_path(name):
    if name.startswith('libs/arm64-v8a/'):
        return ROOT / 'entry' / name
    if name.startswith(RAW):
        return ROOT / 'entry/src/main/resources/rawfile' / name.removeprefix(RAW)
    raise ValueError('Unregistered upstream staging path: ' + name)


def verify_staged(lock):
    return verify(lambda n: staged_path(n).open('rb'), lock['members'], lock)


def stage(hap, lock):
    # Validate every member before changing staging. Never follow a generated
    # symlink into a shared compiler output when replacing a component.
    verify_hap(hap, lock)
    with ZipFile(hap) as archive:
        for name, expected in lock['members'].items():
            path = staged_path(name)
            if path.is_file() and file_sha(path) == expected['sha256']:
                continue
            path.parent.mkdir(parents=True, exist_ok=True)
            pending = path.with_name(path.name + '.selected-upstream.tmp')
            try:
                with archive.open(name) as src, pending.open('wb') as dst:
                    shutil.copyfileobj(src, dst, 1024 * 1024)
                if file_sha(pending) != expected['sha256']:
                    raise ValueError('Upstream staging copy failed: ' + name)
                os.replace(pending, path)
            finally:
                pending.unlink(missing_ok=True)
    return verify_staged(lock)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument('--hap', type=Path)
    group.add_argument('--stage', type=Path)
    group.add_argument('--staged', action='store_true')
    args = parser.parse_args()
    lock = load_lock()
    result = stage(args.stage, lock) if args.stage else verify_hap(args.hap, lock) if args.hap else verify_staged(lock)
    print(json.dumps(dict(status='PASS', **result)))


if __name__ == '__main__':
    main()
