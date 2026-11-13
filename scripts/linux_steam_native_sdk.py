#!/usr/bin/env python3
"""Add a verified Valve x86 SDK to the normal online/offline component plan.

Valve ELF bytes remain in their official ZIP, outside the HAP. This also
upgrades an existing generated plan without rebuilding/copying its runtimes.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import re
import stat
import zipfile

ROOT = Path(__file__).resolve().parents[1]
LOCK = ROOT / 'docs/linux-steam/native-sdk.lock.json'


def encode(value):
    return (json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(',', ':')) + '\n').encode()


def digest(path, algorithm='sha256'):
    result = hashlib.new(algorithm)
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1048576), b''):
            result.update(block)
    return result.hexdigest()


def verify_plan(plan, lock_path=LOCK):
    """Validate the pinned delivery inventory without fetching Valve binaries."""
    lock = json.loads(lock_path.read_text(encoding='utf-8'))
    identity = lock['archive']
    files = [{k: item[k] for k in ['path', 'source', 'bytes', 'sha256']} |
             dict(archive=identity['id'], zipPath=item['source']) for item in lock['files']]
    actual = [f for f in plan['files'] if f['path'].startswith('sdk/') or f['archive'] == identity['id']]
    if sorted(actual, key=lambda f: f['path']) != sorted(files, key=lambda f: f['path']):
        raise ValueError('Native Steam SDK delivery inventory mismatch')
    archives = [a for a in plan['archives'] if a['id'] == identity['id']]
    if archives != [{k: identity[k] for k in ['id', 'name', 'url', 'bytes', 'sha256', 'unpackedBytes']}]:
        raise ValueError('Native Steam SDK delivery archive mismatch')
    release = json.loads(plan['releaseManifest'])
    if (release.get('nativeSteamSdk') != dict(archiveSha256=identity['sha256'], files=files) or
            hashlib.sha256(plan['releaseManifest'].encode()).hexdigest() != plan['id']):
        raise ValueError('Native Steam SDK release identity mismatch')


def apply_sdk(plan, archive, lock_path=LOCK):
    lock = json.loads(lock_path.read_text(encoding='utf-8'))
    identity = lock['archive']
    expected_paths = {'sdk/sdk32/steamclient.so', 'sdk/sdk32/crashhandler.so',
                      'sdk/sdk32/steamerrorreporter', 'sdk/sdk64/steamclient.so', 'sdk/sdk64/crashhandler.so'}
    if (lock['schemaVersion'] != 1 or identity['id'] != 'steam-native-sdk' or
            not re.fullmatch(r'https://client-update\.steamstatic\.com/bins_sdk_linuxarm64\.zip\.[a-f0-9]{40}', identity['url']) or
            len(lock['files']) != 5 or {f['path'] for f in lock['files']} != expected_paths):
        raise ValueError('Invalid native Steam SDK lock')
    if (archive.stat().st_size != identity['bytes'] or digest(archive) != identity['sha256'] or
            digest(archive, 'sha1') != identity['sha1']):
        raise ValueError('Official native Steam SDK archive checksum mismatch')
    files = []
    with zipfile.ZipFile(archive) as source:
        members = source.infolist()
        if len({m.filename for m in members}) != len(members):
            raise ValueError('Duplicate native SDK archive entries')
        if {m.filename for m in members if not m.is_dir()} != {f['source'] for f in lock['files']}:
            raise ValueError('Unexpected native SDK archive entries')
        for item in lock['files']:
            member = source.getinfo(item['source'])
            if stat.S_ISLNK(member.external_attr >> 16) or member.file_size != item['bytes']:
                raise ValueError('Invalid native SDK ZIP member')
            data = source.read(member)
            machine = 3 if item['path'].startswith('sdk/sdk32/') else 62
            if (len(data) < 20 or data[:6] != b'\x7fELF' + bytes([1 if machine == 3 else 2, 1]) or
                    int.from_bytes(data[18:20], 'little') != machine or item['machine'] != machine or
                    hashlib.sha256(data).hexdigest() != item['sha256']):
                raise ValueError('Native SDK ELF/hash mismatch: ' + item['source'])
            files.append({k: item[k] for k in ['path', 'source', 'bytes', 'sha256']} |
                         dict(archive=identity['id'], zipPath=item['source']))
    if sum(f['bytes'] for f in files) != identity['unpackedBytes']:
        raise ValueError('Native SDK unpacked size mismatch')
    result = copy.deepcopy(plan)
    result['files'] = [f for f in result['files'] if not f['path'].startswith('sdk/')]
    result['files'].extend(files)
    result['archives'] = [a for a in result['archives'] if a['id'] != identity['id']]
    result['archives'].append({k: identity[k] for k in ['id', 'name', 'url', 'bytes', 'sha256', 'unpackedBytes']})
    release = json.loads(result['releaseManifest'])
    release['nativeSteamSdk'] = dict(archiveSha256=identity['sha256'], files=files)
    result['releaseManifest'] = encode(release).decode()
    result['id'] = hashlib.sha256(result['releaseManifest'].encode()).hexdigest()
    result['downloadBytes'] = sum(a['bytes'] for a in result['archives'] if a['url'].startswith('https://'))
    result['requiredSpaceBytes'] = (sum(a['bytes'] for a in result['archives']) +
        2 * sum(f['bytes'] for f in result['files']) + max(a['unpackedBytes'] for a in result['archives']) + 536870912)
    verify_plan(result, lock_path)
    return result


def update_plan(plan_path, archive, lock_path=LOCK):
    plan = apply_sdk(json.loads(plan_path.read_text(encoding='utf-8')), archive, lock_path)
    pending = plan_path.with_suffix('.pending')
    pending.write_bytes(encode(plan))
    pending.replace(plan_path)
    return plan


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--plan', type=Path, required=True)
    parser.add_argument('--archive', type=Path, required=True)
    parser.add_argument('--lock', type=Path, default=LOCK)
    args = parser.parse_args()
    plan = update_plan(args.plan, args.archive, args.lock)
    print(json.dumps(dict(id=plan['id'], sdkFiles=5, officialArchives=len(plan['archives'])-1)))
