# SPDX-License-Identifier: LGPL-2.1-or-later
"""Lock the complete Wine module directories from one verified GE archive."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import tarfile

DIRECTORIES = tuple('files/lib/wine/'+arch for arch in
                    ('aarch64-windows', 'i386-windows', 'aarch64-unix'))


def sha_file(path, kind):
    value = hashlib.new(kind)
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024*1024), b''):
            value.update(chunk)
    return value.hexdigest()


def directory_receipt(rows):
    rows = sorted(rows, key=lambda row: row['path'])
    data = json.dumps([[row['path'], row['bytes'], row['sha256']] for row in rows],
                      separators=(',', ':')).encode()
    return dict(files=len(rows), bytes=sum(row['bytes'] for row in rows),
                membersSha256=hashlib.sha256(data).hexdigest())


def validate_module_lock(lock):
    closure = lock.get('wineModuleClosure', {})
    if closure.get('schema') != 1 or set(closure.get('directories', {})) != set(DIRECTORIES):
        raise ValueError('Complete same-distribution Wine module inventory required')
    selected = lock['selected']
    if len({row['path'] for row in selected}) != len(selected):
        raise ValueError('Duplicate selected GE path')
    for directory in DIRECTORIES:
        rows = [row for row in selected if row['path'].startswith(directory+'/')]
        if not rows or directory_receipt(rows) != closure['directories'][directory]:
            raise ValueError('Wine module directory incomplete or changed: '+directory)


def complete_module_lock(lock, archive):
    """Read all modules, never extract an unchecked archive or modify a cache."""
    archive = Path(archive)
    identity = lock['distribution']
    if (archive.stat().st_size != identity['bytes'] or
            sha_file(archive, 'sha512') != identity['sha512']):
        raise ValueError('GE archive identity mismatch')
    prefix = identity['file'].removesuffix('.tar.gz')+'/'
    modules = {}
    with tarfile.open(archive, 'r|gz') as source:
        for member in source:
            if not member.name.startswith(prefix):
                continue
            name = member.name[len(prefix):]
            if not any(name.startswith(directory+'/') for directory in DIRECTORIES):
                continue
            if member.isdir():
                continue
            path = PurePosixPath(name)
            if (path.as_posix() != name or '..' in path.parts or '\\' in name or
                    name in modules or not member.isfile() or not 0 < member.size <= 128*1024*1024):
                raise ValueError('Unsafe or duplicate Wine module: '+name)
            value = hashlib.sha256()
            with source.extractfile(member) as stream:
                for chunk in iter(lambda: stream.read(1024*1024), b''):
                    value.update(chunk)
            modules[name] = dict(path=name, bytes=member.size, type='0', link='', sha256=value.hexdigest())
    selected = {row['path']: row for row in lock['selected']}
    if len(selected) != len(lock['selected']):
        raise ValueError('Duplicate selected GE path')
    for name, row in selected.items():
        if any(name.startswith(directory+'/') for directory in DIRECTORIES):
            if name not in modules or any(row[key] != modules[name][key] for key in ('bytes', 'sha256')):
                raise ValueError('Selected module differs from locked distribution: '+name)
    selected.update(modules)
    result = dict(lock, selected=sorted(selected.values(), key=lambda row: row['path']))
    result['selectedBytes'] = sum(row['bytes'] for row in result['selected'])
    result['wineModuleClosure'] = dict(schema=1, source='complete directories from locked GE archive',
        directories={directory: directory_receipt([row for name, row in modules.items()
                                                  if name.startswith(directory+'/')])
                     for directory in DIRECTORIES})
    validate_module_lock(result)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--lock', type=Path, required=True)
    parser.add_argument('--archive', type=Path, required=True)
    parser.add_argument('--write-lock', action='store_true')
    args = parser.parse_args()
    original = json.loads(args.lock.read_text())
    completed = complete_module_lock(original, args.archive)
    if args.write_lock:
        args.lock.write_text(json.dumps(completed, indent=2)+'\n')
    elif completed != original:
        raise ValueError('GE module lock does not cover the complete distribution directories')
    print(json.dumps(completed['wineModuleClosure'], sort_keys=True))


if __name__ == '__main__':
    main()
