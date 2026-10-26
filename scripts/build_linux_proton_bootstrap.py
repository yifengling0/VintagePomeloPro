#!/usr/bin/env python3
"""Deliver the small 64-bit Steam helper/service closure independently of GE.

Uses only members from the already pinned upstream GE archive. The immutable
main component and its offline imports remain usable; no runtime republishing.
"""
import argparse
import hashlib
import json
import tarfile
from pathlib import Path, PurePosixPath

ROOT = Path(__file__).resolve().parents[1]
NAMES = ('lsass.exe', 'psapi.dll', 'wevtsvc.dll')


def sha(path, algorithm='sha256'):
    h = hashlib.new(algorithm)
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''): h.update(block)
    return h.hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--archive', type=Path, required=True)
    p.add_argument('--output', type=Path, default=ROOT/'entry/src/main/resources/rawfile/linux-proton/bootstrap')
    a = p.parse_args()
    dist = json.loads((ROOT/'docs/linux-steam/gnu-proton-probe.lock.json').read_text())['distribution']
    manifest = a.output/'manifest.json'
    old = json.loads(manifest.read_text()) if manifest.exists() else {}
    if old.get('distributionSha512') == dist['sha512'] and {r['name'] for r in old.get('files', [])} == set(NAMES):
        if all((a.output/r['name']).is_file() and sha(a.output/r['name']) == r['sha256'] for r in old['files']):
            print('Reused verified Proton bootstrap closure'); return
    if a.archive.stat().st_size != dist['bytes'] or sha(a.archive, 'sha512') != dist['sha512']:
        raise ValueError('GE distribution identity mismatch')
    a.output.mkdir(parents=True, exist_ok=True)
    rows = []
    with tarfile.open(a.archive, 'r|gz') as archive:
        for member in archive:
            parts = PurePosixPath(member.name).parts
            if len(parts) != 6 or parts[1:5] != ('files', 'lib', 'wine', 'aarch64-windows') or parts[-1] not in NAMES:
                continue
            if not member.isfile() or not 0 < member.size < 2*1024*1024: raise ValueError('Invalid bootstrap member')
            data = archive.extractfile(member).read()
            (a.output/parts[-1]).write_bytes(data)
            rows.append(dict(name=parts[-1], sourcePath='/'.join(parts[1:]), bytes=len(data), sha256=hashlib.sha256(data).hexdigest()))
    if {r['name'] for r in rows} != set(NAMES): raise ValueError('Incomplete bootstrap closure')
    manifest.write_text(json.dumps(dict(schemaVersion=1, architecture='aarch64-windows',
        distributionSha512=dist['sha512'], files=sorted(rows, key=lambda r:r['name'])), indent=2)+'\n')
    print('Verified Proton bootstrap closure:', len(rows), 'files,', sum(r['bytes'] for r in rows), 'bytes')


if __name__ == '__main__': main()
