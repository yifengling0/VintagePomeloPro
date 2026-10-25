#!/usr/bin/env python3
"""Extract only the locked GNU Proton probe files; never unpacks a full rootfs."""
from pathlib import Path, PurePosixPath
import argparse, hashlib, json, tarfile
ROOT = Path(__file__).resolve().parents[1]

def sha(path, algorithm='sha256'):
    digest = hashlib.new(algorithm)
    with path.open('rb') as f:
        for chunk in iter(lambda: f.read(1024*1024), b''): digest.update(chunk)
    return digest.hexdigest()

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--archive', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True, help='Reusable task directory outside the source tree')
    p.add_argument('--lock', type=Path, default=ROOT/'docs/linux-steam/gnu-proton-probe.lock.json')
    a = p.parse_args()
    lock = json.loads(a.lock.read_text())
    output = a.output.resolve()
    if output.is_relative_to(ROOT): p.error('Keep extracted third-party binaries outside the repository')
    output.mkdir(parents=True, exist_ok=True)
    selected = {}
    for row in lock['selected']:
        path = PurePosixPath(row['path'])
        if path.is_absolute() or '..' in path.parts or not path.parts or row['bytes'] > 64*1024*1024:
            raise ValueError('Unsafe locked path or file size')
        name = str(path)
        if name in selected: raise ValueError('Duplicate locked path')
        target = output.joinpath(*path.parts)
        if not target.resolve().is_relative_to(output): raise ValueError('Output symlink escapes task directory')
        selected[name] = row
    def matches(name, row):
        path = output/name
        return path.is_file() and not path.is_symlink() and path.stat().st_size == row['bytes'] and sha(path) == row['sha256']
    needed = {name for name,row in selected.items() if not matches(name,row)}
    if not needed:
        print('Reused',len(selected),'verified GNU Proton probe files')
        return
    archive = a.archive.resolve(strict=True)
    dist = lock['distribution']
    if archive.stat().st_size != dist['bytes'] or sha(archive,'sha512') != dist['sha512']:
        raise ValueError('GNU Proton archive identity mismatch')
    with tarfile.open(archive,'r|gz') as stream:
        for member in stream:
            parts = PurePosixPath(member.name).parts
            if not parts or parts[0] != dist['file'].removesuffix('.tar.gz'): continue
            name = str(PurePosixPath(*parts[1:]))
            if name not in needed: continue
            row = selected[name]
            if not member.isfile() or member.size != row['bytes']: raise ValueError('Locked member type/size mismatch')
            data = stream.extractfile(member).read()
            if hashlib.sha256(data).hexdigest() != row['sha256']: raise ValueError('Locked member checksum mismatch')
            target = output/name
            target.parent.mkdir(parents=True,exist_ok=True)
            temporary = target.with_name(target.name+'.probe-partial')
            if temporary.is_symlink(): raise ValueError('Unsafe partial output')
            temporary.write_bytes(data)
            temporary.replace(target)
            needed.remove(name)
    if needed: raise ValueError('Missing locked members: '+', '.join(sorted(needed)))
    print('Verified',len(selected),'GNU Proton probe files,',lock['selectedBytes'],'bytes')

if __name__ == '__main__': main()
