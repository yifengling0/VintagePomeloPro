#!/usr/bin/env python3
"""Materialize the small, pinned GNU Venus test input without touching OHOS Mesa.

The production OHOS build applies different managed Mesa patches in place.
Runtime regressions must exercise the locked GNU variant on fresh and incremental
builds alike. The existing GNU builder still verifies every effective file hash.
"""
import json
import subprocess
from pathlib import Path, PurePosixPath

ROOT = Path(__file__).resolve().parents[1]


def prepare():
    lock = json.loads((ROOT / 'docs/linux-steam/gnu-venus-base.lock.json').read_text())
    revision = lock['mesaCommit']
    row = subprocess.check_output(
        ['git', 'ls-tree', 'HEAD', 'thirdparty/mesa'], cwd=ROOT, text=True).split()
    if len(row) < 3 or row[:2] != ['160000', 'commit'] or row[2] != revision:
        raise ValueError('GNU Venus lock does not match the committed Mesa gitlink')
    source = ROOT / 'thirdparty/mesa'
    target = ROOT / 'workspace_temp/hsl-sync/mesa-test-base'
    if target.is_symlink():
        raise ValueError('GNU Venus test input must be a private directory')
    total = 0
    for name in sorted(lock['files']):
        relative = PurePosixPath(name)
        if relative.is_absolute() or '..' in relative.parts:
            raise ValueError('Invalid locked Mesa path')
        data = subprocess.check_output(['git', '-C', str(source), 'show', revision + ':' + name])
        destination = target / name
        for parent in (destination, *destination.parents):
            if parent.is_symlink():
                raise ValueError('GNU Venus test input must not follow symlinks')
            if parent == ROOT:
                break
        destination.parent.mkdir(parents=True, exist_ok=True)
        if not destination.is_file() or destination.read_bytes() != data:
            destination.write_bytes(data)
        total += len(data)
    print(f'Pinned GNU Venus test input: {revision} files={len(lock["files"])} bytes={total}')


if __name__ == '__main__':
    prepare()
