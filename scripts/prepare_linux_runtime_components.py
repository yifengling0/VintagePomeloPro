#!/usr/bin/env python3
"""Restore pinned GNU dependencies for a clean HAP checkout; never use local task paths.

The OHOS host and ArkTS application are compiled by make. GNU binary dependencies
are distributed separately with their source contracts and exact member hashes.
This is not a claim that a HAP build recompiles GE Wine/glibc/Mesa from source.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import stat
import subprocess
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
LOCK = ROOT / 'docs/linux-steam/build-components.lock.json'
DESTINATIONS = {
    'linux-steam/hmosdeck-gnu-arm64-v1.zip': 'linux-steam/deployment.json',
    'linux-proton/gnu-proton.zip': 'linux-proton/manifest.json',
}


def digest(stream):
    h = hashlib.sha256()
    for block in iter(lambda: stream.read(1024 * 1024), b''): h.update(block)
    return h.hexdigest()


def sha(path):
    with path.open('rb') as stream: return digest(stream)


def safe_path(name):
    if (not name or name.startswith('/') or '\\' in name or ':' in name or
            any(p in ('', '.', '..') for p in name.split('/')) or
            any(ord(c) < 32 for c in name)):
        raise ValueError('Unsafe component member: ' + name)
    return PurePosixPath(name)


def verify_sources(root, lock):
    for name, expected in lock['sourceInputs'].items():
        safe_path(name)
        source = root / name
        if not source.is_file() or sha(source) != expected:
            raise ValueError('GNU component source changed: ' + name +
                             '; rebuild and publish the matching dependency, then update the lock')
    for name, expected in lock.get('gitlinks',{}).items():
        safe_path(name)
        actual=subprocess.check_output(['git','-C',str(root),'rev-parse','HEAD:thirdparty/'+name],text=True).strip()
        if actual != expected: raise ValueError('GNU component gitlink changed: '+name)


def expected_members(root, row):
    raw = root / 'entry/src/main/resources/rawfile'
    manifest_path = raw / DESTINATIONS[row['path']]
    if sha(manifest_path) != row['manifestSha256']:
        raise ValueError('Component manifest changed without build lock: ' + row['path'])
    manifest = json.loads(manifest_path.read_text())
    if row['path'].startswith('linux-proton/'):
        if manifest['sha256'] != row['sha256'] or manifest['bytes'] != row['bytes']:
            raise ValueError('Proton archive identity differs from build lock')
        rows = manifest['files']
    else:
        archive = next(a for a in manifest['archives'] if a['id'] == 'gnu-runtime')
        if archive['sha256'] != row['sha256'] or archive['bytes'] != row['bytes']:
            raise ValueError('Steam GNU archive identity differs from build lock')
        rows = [dict(path=r['zipPath'],bytes=r['bytes'],sha256=r['sha256'])
                for r in manifest['files'] if r['archive'] == 'gnu-runtime']
        # Valve client resources are fetched from Valve by the app, not here.
        if any(not (r['path'].startswith('release/runtime/') and '/steamrtarm64/' not in r['path'] or
                    r['path'] == 'client/steamohos/ca-certificates.crt') for r in rows):
            raise ValueError('Unexpected client payload in project runtime')
    result = {}
    for member in rows:
        safe_path(member['path'])
        if member['path'] in result: raise ValueError('Duplicate manifest member')
        result[member['path']] = member
    return result


def verify_archive(path, row, members):
    if path.stat().st_size != row['bytes'] or sha(path) != row['sha256']:
        raise ValueError('Component archive identity mismatch: ' + row['path'])
    with zipfile.ZipFile(path) as archive:
        names = archive.namelist()
        if len(names) != len(set(names)) or set(names) != set(members):
            raise ValueError('Component archive inventory mismatch')
        for name in names:
            safe_path(name)
            info = archive.getinfo(name)
            if stat.S_ISLNK(info.external_attr >> 16) or info.is_dir():
                raise ValueError('Component must contain regular files')
            member = members[name]
            if info.file_size != member['bytes']:
                raise ValueError('Component member size mismatch: ' + name)
            with archive.open(info) as stream:
                if digest(stream) != member['sha256']:
                    raise ValueError('Component member checksum mismatch: ' + name)


def prepare(root, lock, cache, verify_only=False, opener=urllib.request.urlopen):
    if lock.get('schemaVersion') != 1: raise ValueError('Unsupported component lock')
    rows = lock['components']
    if {r['path'] for r in rows} != set(DESTINATIONS) or len(rows) != len(DESTINATIONS):
        raise ValueError('Both GNU component dependencies are required')
    verify_sources(root, lock)
    report = []
    for row in rows:
        members = expected_members(root, row)
        target = root / 'entry/src/main/resources/rawfile' / row['path']
        if target.is_symlink(): raise ValueError('Linked rawfile component rejected')
        if target.is_file() and target.stat().st_size == row['bytes'] and sha(target) == row['sha256']:
            verify_archive(target,row,members)
            report.append(dict(path=row['path'],sha256=row['sha256'],source='existing_verified_rawfile'))
            continue
        if verify_only: raise ValueError('Missing or corrupt GNU component: ' + row['path'])
        cache.mkdir(parents=True,exist_ok=True)
        saved = cache / (row['sha256'] + '.zip')
        if saved.is_symlink(): raise ValueError('Linked dependency cache rejected')
        if not saved.is_file() or saved.stat().st_size != row['bytes'] or sha(saved) != row['sha256']:
            url = row['url']
            if not url.startswith('https://github.com/yifengling0/VintagePomeloPro/releases/download/'):
                raise ValueError('Unexpected dependency URL')
            temporary = saved.with_suffix('.part')
            if temporary.is_symlink(): raise ValueError('Linked download rejected')
            print('Downloading pinned GNU dependency:',row['path'],flush=True)
            request = urllib.request.Request(url,headers={'User-Agent':'VintagePomeloPro-build/1'})
            with opener(request,timeout=90) as response, temporary.open('wb') as output:
                count = 0
                while block := response.read(1024 * 1024):
                    count += len(block)
                    if count > row['bytes']: raise ValueError('Oversized component download')
                    output.write(block)
            verify_archive(temporary,row,members)
            os.replace(temporary,saved)
        else:
            verify_archive(saved,row,members)
        target.parent.mkdir(parents=True,exist_ok=True)
        pending = target.with_suffix('.part')
        if pending.is_symlink(): raise ValueError('Linked rawfile staging rejected')
        shutil.copyfile(saved,pending)
        if sha(pending) != row['sha256']: raise ValueError('Component staging mismatch')
        os.replace(pending,target)
        report.append(dict(path=row['path'],sha256=row['sha256'],source='pinned_dependency'))
    return dict(schemaVersion=1,status='PASS',gnuDependencies=report,
                gnuDependenciesBuiltFromSource=False,ohosHostBuildPolicy='source')


def verify_hap(path, root=ROOT):
    lock = json.loads((root/'docs/linux-steam/build-components.lock.json').read_text())
    verify_sources(root,lock)
    with zipfile.ZipFile(path) as archive:
        for row in lock['components']:
            name = 'resources/rawfile/' + row['path']
            info = archive.getinfo(name)
            if info.file_size != row['bytes']: raise ValueError('HAP GNU component size mismatch')
            with archive.open(info) as stream:
                if digest(stream) != row['sha256']: raise ValueError('HAP GNU component checksum mismatch')
            name = 'resources/rawfile/' + DESTINATIONS[row['path']]
            if hashlib.sha256(archive.read(name)).hexdigest() != row['manifestSha256']:
                raise ValueError('HAP GNU manifest mismatch')
    return dict(status='PASS',components=len(lock['components']))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--verify-only',action='store_true')
    p.add_argument('--hap',type=Path)
    p.add_argument('--cache',type=Path,default=ROOT/'build/linux-runtime-downloads')
    p.add_argument('--report',type=Path)
    args = p.parse_args()
    result = verify_hap(args.hap) if args.hap else prepare(ROOT,json.loads(LOCK.read_text()),args.cache,args.verify_only)
    if args.report:
        args.report.parent.mkdir(parents=True,exist_ok=True)
        args.report.write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result),flush=True)


if __name__ == '__main__': main()
