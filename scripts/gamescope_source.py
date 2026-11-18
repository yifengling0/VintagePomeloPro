#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Replay the reviewed Gamescope recipe without GNU runtime dependencies."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

from gamescope_prepare_source import ROOT, inventory, prepare, relative


def digest(path):
    return hashlib.sha256(path.read_bytes().replace(b'\r\n', b'\n')).hexdigest()


def load(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def publish_receipt(path, value):
    # A failed write must not destroy the receipt for the source we roll back.
    temporary = path.with_suffix('.pending')
    try:
        temporary.write_text(json.dumps(value, indent=2)+'\n')
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)


def recipe_inventory(target):
    files = inventory(target)
    # Meson creates this empty advisory lock even with --wrap-mode=nodownload.
    # Ignore exactly that generated file; all source changes remain checked.
    lock = files.pop('subprojects/.wraplock', None)
    if lock and lock['bytes'] != 0:
        raise ValueError('Unexpected Meson wrap lock content')
    # Meson promotes this nested wrap into the parent subprojects directory.
    # Accept only its exact redirect to the locked subproject source.
    promoted = files.get('subprojects/v4l-utils.wrap')
    if promoted:
        if ((target/'subprojects/v4l-utils.wrap').read_bytes() !=
                b'[wrap-redirect]\nfilename = libdisplay-info/subprojects/v4l-utils.wrap\n'):
            raise ValueError('Unexpected Meson promoted wrap content')
        files.pop('subprojects/v4l-utils.wrap')
    return files


def prepare_recipe(cache, work, refresh=False):
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
        if saved['inputs'] == identity and saved['files'] == recipe_inventory(target):
            return target
        previous = saved['inputs']
        old = previous['recipe']
        if refresh and saved['files'] == recipe_inventory(target) and previous['sourceLock'] == lock:
            # Recompose only declared patch/overlay files from verified pristine
            # inputs. A changed reviewed patch never forces another full tree.
            paths = {str(relative(item['originalPath'])) for r in [*old['patches'], *recipe['patches']]
                     for item in r.get('files', [r])}
            paths.update(str(relative(r['destination'])) for r in [*old['overlays'], *recipe['overlays']])
            with tempfile.TemporaryDirectory(prefix='recipe-', dir=work) as directory:
                stage = Path(directory)
                for p in paths:
                    if (pristine/p).is_file():
                        (stage/p).parent.mkdir(parents=True, exist_ok=True)
                        shutil.copy2(pristine/p, stage/p)
                apply_recipe(stage, recipe['patches'], recipe['overlays'])
                before = {p: ((target/p).read_bytes(), (target/p).stat().st_mode)
                          if (target/p).is_file() else None for p in paths}
                try:
                    for p in paths:
                        if (stage/p).is_file():
                            (target/p).parent.mkdir(parents=True, exist_ok=True)
                            shutil.copy2(stage/p, target/p)
                        else:
                            (target/p).unlink(missing_ok=True)
                    publish_receipt(receipt, {'inputs': identity, 'files': recipe_inventory(target)})
                except BaseException:
                    for p, value in before.items():
                        if value is None:
                            (target/p).unlink(missing_ok=True)
                        else:
                            (target/p).write_bytes(value[0])
                            (target/p).chmod(value[1])
                    raise
            return target
        # An append-only reviewed platform recipe can reuse the existing tree.
        # Unknown source edits and changes to an earlier patch are never reset.
        if (saved['files'] != recipe_inventory(target) or previous['sourceLock'] != lock or
                recipe['patches'][:len(old['patches'])] != old['patches'] or
                recipe['overlays'][:len(old['overlays'])] != old['overlays']):
            raise ValueError('Prepared recipe changed incompatibly; existing source preserved')
        patches = recipe['patches'][len(old['patches']):]
        overlays = recipe['overlays'][len(old['overlays']):]
        paths = {str(relative(item['originalPath'])) for record in patches
                 for item in record.get('files', [record])}
        paths.update(str(relative(r['destination'])) for r in overlays)
        # Keep only the touched files for rollback, never another whole tree.
        before = {p: ((target/p).read_bytes(), (target/p).stat().st_mode)
                  if (target/p).is_file() else None for p in paths}
        try:
            apply_recipe(target, patches, overlays)
            publish_receipt(receipt, {'inputs': identity, 'files': recipe_inventory(target)})
        except BaseException:
            for p, value in before.items():
                path = target/p
                if value is None:
                    path.unlink(missing_ok=True)
                else:
                    path.write_bytes(value[0])
                    path.chmod(value[1])
            raise
        return target
    if target.exists():
        raise ValueError('Unreceipted Gamescope source preserved; inspect before retrying')
    shutil.copytree(pristine, target)
    apply_recipe(target, recipe['patches'], recipe['overlays'])
    publish_receipt(receipt, {'inputs': identity, 'files': recipe_inventory(target)})
    return target


def apply_recipe(target, patches, overlays):
    for record in patches:
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
    for record in overlays:
        path = target/str(relative(record['destination']))
        if path.exists():
            raise ValueError('Overlay would replace upstream source: '+record['destination'])
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes((ROOT/str(relative(record['source']))).read_bytes().replace(b'\r\n', b'\n'))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cache', type=Path, default=ROOT/'build/gamescope/cache')
    parser.add_argument('--work', type=Path, default=ROOT/'build/gamescope/arm64-v8a')
    parser.add_argument('--refresh-reviewed', action='store_true',
                        help='Recompose changed reviewed patches only when the previous source receipt is intact')
    args = parser.parse_args()
    print(prepare_recipe(args.cache.resolve(), args.work.resolve(), args.refresh_reviewed))
