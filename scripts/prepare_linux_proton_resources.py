#!/usr/bin/env python3
"""Lock and stage the actual resources referenced by Wine's installation INF."""
from pathlib import Path, PurePosixPath
import argparse
import hashlib
import json
import re
import tarfile
from prepare_linux_proton_probe import sha


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--archive', type=Path, required=True)
    p.add_argument('--package', type=Path, required=True)
    p.add_argument('--lock', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    lock = json.loads(a.lock.read_text())
    dist = lock['distribution']
    if a.archive.stat().st_size != dist['bytes'] or sha(a.archive, 'sha512') != dist['sha512']:
        raise ValueError('GNU Proton archive identity mismatch')
    text = (a.package/'files/share/wine/wine.inf').read_text()
    sections = {}
    section = None
    for line in text.splitlines():
        line = line.split(';', 1)[0].strip()
        if line.startswith('[') and line.endswith(']'):
            section = line[1:-1]
            sections[section] = []
        elif section and line:
            sections[section].append(line)
    wanted = set()
    for entry in sections['WineSourceDirs']:
        section, directory = entry.split('=', 1)
        # The optional second directory is a build-tree fallback. Distributed
        # Proton packages use the first path under share/wine.
        directory = directory.split(',', 1)[0].strip()
        for line in sections[section.strip()]:
            name = line.split(',', 1)[0].strip().strip('"')
            path = PurePosixPath('files/share/wine')/directory/name
            if path.is_absolute() or '..' in path.parts:
                raise ValueError('Unsafe INF resource path')
            wanted.add(str(path))
    records = {row['path']: row for row in lock['selected']}
    if wanted <= records.keys() and all((a.package/name).is_file() and
            sha(a.package/name) == records[name]['sha256'] for name in wanted):
        a.output.write_text(json.dumps([records[name] for name in sorted(wanted)], indent=2)+'\n')
        print('Reused verified INF resources:', len(wanted))
        return
    found = {}
    with tarfile.open(a.archive, 'r|gz') as stream:
        for member in stream:
            parts = PurePosixPath(member.name).parts
            if not parts or parts[0] != dist['file'].removesuffix('.tar.gz'):
                continue
            name = str(PurePosixPath(*parts[1:]))
            if name not in wanted:
                continue
            if not member.isfile() or member.size > 32*1024*1024:
                raise ValueError('Unexpected resource type or size')
            data = stream.extractfile(member).read()
            digest = hashlib.sha256(data).hexdigest()
            target = a.package/name
            if not target.resolve().is_relative_to(a.package.resolve()) or target.is_symlink():
                raise ValueError('Unsafe resource output')
            if target.exists() and sha(target) != digest:
                raise ValueError('Existing resource differs: '+name)
            target.parent.mkdir(parents=True, exist_ok=True)
            if not target.exists():
                target.write_bytes(data)
            row = {'path': name, 'bytes': len(data), 'type': '0', 'link': '', 'sha256': digest}
            found[name] = row
            if name not in records:
                lock['selected'].append(row)
    if wanted != found.keys():
        raise ValueError('Missing distributed INF resources: '+str(sorted(wanted-found.keys())))
    lock['selectedBytes'] = sum(row['bytes'] for row in lock['selected'] if row['type'] == '0')
    a.lock.write_text(json.dumps(lock, indent=2)+'\n')
    (a.output.parent/'package-bootstrap-inventory.json').write_text(json.dumps(lock, indent=2)+'\n')
    a.output.write_text(json.dumps([found[name] for name in sorted(found)], indent=2)+'\n')
    print('Verified INF resources:', len(found), 'bytes:', sum(row['bytes'] for row in found.values()))


if __name__ == '__main__':
    main()
