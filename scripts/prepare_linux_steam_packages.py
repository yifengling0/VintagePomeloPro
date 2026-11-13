#!/usr/bin/env python3
"""Verify and extract pinned official ARM64 Steam packages into a new private tree."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import zipfile


def sha(path):
    with path.open('rb') as stream:
        digest = hashlib.sha256()
        for block in iter(lambda: stream.read(4 * 1024 * 1024), b''):
            digest.update(block)
        return digest.hexdigest()


def relative_path(value):
    normalized = value.replace('\\', '/')
    path = PurePosixPath(normalized)
    if path.is_absolute() or '..' in path.parts or ':' in normalized or '\x00' in normalized or not path.parts:
        raise ValueError('Invalid archive path: ' + repr(value))
    return path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--lock', type=Path, required=True)
    parser.add_argument('--downloads', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    lock = json.loads(args.lock.read_text())
    checked = {}
    for name, record in lock['packages'].items():
        filename = record['file']
        if Path(filename).name != filename or not re.fullmatch(r'[a-f0-9]{64}', record['sha2']):
            parser.error('Invalid package lock: ' + name)
        archive = args.downloads / filename
        if sha(archive) != record['sha2'] or archive.stat().st_size != int(record['size']):
            parser.error('Official package identity mismatch: ' + name)
        checked[name] = record['sha2']
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    root = output / 'steam-root'
    root.mkdir()
    links = {}
    for record in lock['packages'].values():
        with zipfile.ZipFile(args.downloads / record['file']) as archive:
            for member in archive.infolist():
                relative = relative_path(member.filename)
                target = root / relative
                if stat.S_ISLNK(member.external_attr >> 16):
                    value = archive.read(member).decode('utf-8').replace('\\', '/')
                    if value.startswith('/') or '\x00' in value or not (target.parent / value).resolve().is_relative_to(root):
                        parser.error('External archive link: ' + member.filename)
                    links[str(relative)] = value
                    continue
                if member.is_dir() or member.filename.endswith(('/', '\\')):
                    target.mkdir(parents=True, exist_ok=True)
                    continue
                target.parent.mkdir(parents=True, exist_ok=True)
                with archive.open(member) as source, target.open('wb') as destination:
                    shutil.copyfileobj(source, destination)
    for relative, value in links.items():
        target = root / relative
        # No previously extracted directory may be replaced by a link.
        if target.exists():
            parser.error('Archive link collides with regular data: ' + relative)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.symlink_to(value)
        if not target.resolve().is_relative_to(root):
            parser.error('Archive link chain leaves the private tree')
    result = {'packagesLockSha256': sha(args.lock), 'version': lock['version'],
              'verifiedArchives': checked, 'symlinks': links,
              'files': {str(p.relative_to(root)): sha(p) for p in sorted(root.rglob('*')) if p.is_file()}}
    (output / 'extraction.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({'root': str(root), 'verifiedArchives': len(checked), 'files': len(result['files'])}))


if __name__ == '__main__':
    main()
