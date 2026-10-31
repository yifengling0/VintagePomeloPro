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
NAMES = ('dwmapi.dll', 'dwrite.dll', 'lsass.exe', 'mswsock.dll', 'oleacc.dll',
         'psapi.dll', 'uiautomationcore.dll', 'wevtsvc.dll', 'wsock32.dll')


def sha(path, algorithm='sha256'):
    h = hashlib.new(algorithm)
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''): h.update(block)
    return h.hexdigest()


def verify(root=ROOT):
    from prepare_linux_proton_modules import imports
    from fex_simd_runtime import pe_kind
    output = root/'entry/src/main/resources/rawfile/linux-proton/bootstrap'
    manifest = json.loads((output/'manifest.json').read_text())
    upstream = json.loads((root/'docs/linux-steam/gnu-proton-probe.lock.json').read_text())
    media = json.loads((root/'docs/linux-steam/proton-media.lock.json').read_text())
    if (manifest['schemaVersion'] != 1 or manifest['architecture'] != 'aarch64-windows' or
            manifest['distributionSha512'] != upstream['distribution']['sha512'] or
            len(manifest['files']) != len(NAMES) or {r['name'] for r in manifest['files']} != set(NAMES)):
        raise ValueError('Proton bootstrap inventory/source mismatch')
    # PE forwarders need their target DLLs too. Check the real 64-bit package,
    # media supplement and bootstrap together, never the developer's prefix.
    available = {PurePosixPath(r['path']).name for r in upstream['selected'] + media['files']
                 if '/wine/aarch64-windows/' in r['path']} | set(NAMES)
    for row in manifest['files']:
        path = output/row['name']
        if (row['sourcePath'] != 'files/lib/wine/aarch64-windows/'+row['name'] or
                path.is_symlink() or not 0 < row['bytes'] < 2*1024*1024 or
                path.stat().st_size != row['bytes'] or sha(path) != row['sha256']):
            raise ValueError('Proton bootstrap member identity: '+row['name'])
        data = path.read_bytes()
        if pe_kind(data) not in ('arm64', 'arm64ec', 'arm64x'):
            raise ValueError('Proton bootstrap PE architecture: '+row['name'])
        missing = {n for n in imports(data) - available if not n.startswith(('api-ms-', 'ext-ms-'))}
        if missing: raise ValueError('Missing Proton bootstrap imports: '+str(sorted(missing)))
    return manifest


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--archive', type=Path)
    p.add_argument('--verify-only', action='store_true')
    p.add_argument('--output', type=Path, default=ROOT/'entry/src/main/resources/rawfile/linux-proton/bootstrap')
    a = p.parse_args()
    if a.verify_only:
        verify(); print('Verified Proton bootstrap identities and PE dependency closure'); return
    if not a.archive: p.error('Provide --archive, or use --verify-only')
    dist = json.loads((ROOT/'docs/linux-steam/gnu-proton-probe.lock.json').read_text())['distribution']
    manifest = a.output/'manifest.json'
    old = json.loads(manifest.read_text()) if manifest.exists() else {}
    if old.get('distributionSha512') == dist['sha512'] and {r['name'] for r in old.get('files', [])} == set(NAMES):
        if all((a.output/r['name']).is_file() and sha(a.output/r['name']) == r['sha256'] for r in old['files']):
            if a.output.resolve() == (ROOT/'entry/src/main/resources/rawfile/linux-proton/bootstrap').resolve(): verify()
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
    if a.output.resolve() == (ROOT/'entry/src/main/resources/rawfile/linux-proton/bootstrap').resolve(): verify()
    print('Verified Proton bootstrap closure:', len(rows), 'files,', sum(r['bytes'] for r in rows), 'bytes')


if __name__ == '__main__': main()
