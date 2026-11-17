#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Replay the reviewed Gamescope recipe without GNU runtime dependencies."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

from gamescope_prepare_source import ROOT, inventory, prepare, relative


def digest(path):
    return hashlib.sha256(path.read_bytes().replace(b'\r\n', b'\n')).hexdigest()


def load(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def prepare_recipe(cache, work):
    recipe = load(ROOT/'components/gamescope-ohos.lock.json')
    lock = load(ROOT/str(relative(recipe['sourceLock'])))
    # Validate all inputs before any source mutation.
    for record in recipe['patches']:
        if digest(ROOT/str(relative(record['path']))) != record['sha256']:
            raise ValueError('Patch identity mismatch: '+record['path'])
    for record in recipe['overlays']:
        if digest(ROOT/str(relative(record['source']))) != record['sha256']:
            raise ValueError('Overlay identity mismatch: '+record['source'])
    pristine = prepare(lock, cache, work/'pristine')
    target = work/'source'
    receipt = work/'source-receipt.json'
    identity = {'recipe': recipe, 'sourceLock': lock}
    if receipt.exists():
        saved = load(receipt)
        if saved['inputs'] == identity and saved['files'] == inventory(target):
            return target
        raise ValueError('Prepared recipe changed; preserve existing source and inspect the difference')
    if target.exists():
        raise ValueError('Unreceipted Gamescope source preserved; inspect before retrying')
    shutil.copytree(pristine, target)
    for record in recipe['patches']:
        affected = record.get('files', [record])
        for item in affected:
            if digest(target/str(relative(item['originalPath']))) != item['beforeSha256']:
                raise ValueError('Before-patch identity mismatch: '+item['originalPath'])
        result = subprocess.run(['patch', '-p1', '--batch', '--fuzz=0', '-i',
                                 str(ROOT/str(relative(record['path'])))],
                                cwd=target, capture_output=True, text=True, timeout=30)
        if result.returncode:
            raise ValueError('Patch failed: '+record['path']+'\n'+result.stdout+result.stderr)
        for item in affected:
            if digest(target/str(relative(item['originalPath']))) != item['afterSha256']:
                raise ValueError('After-patch identity mismatch: '+item['originalPath'])
    for record in recipe['overlays']:
        path = target/str(relative(record['destination']))
        if path.exists():
            raise ValueError('Overlay would replace upstream source: '+record['destination'])
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes((ROOT/str(relative(record['source']))).read_bytes().replace(b'\r\n', b'\n'))
    receipt.write_text(json.dumps({'inputs': identity, 'files': inventory(target)}, indent=2)+'\n')
    return target


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cache', type=Path, default=ROOT/'build/gamescope/cache')
    parser.add_argument('--work', type=Path, default=ROOT/'build/gamescope/arm64-v8a')
    args = parser.parse_args()
    print(prepare_recipe(args.cache.resolve(), args.work.resolve()))
