#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Acquire unchanged, hash-pinned WineHua DXVK source trees for HSL builds.

--output is a NEW source root containing legacy/, modern/, wine/ and
source-inputs.json. Existing output is only verified, never updated in place.
Toolchain/package pins are retained in the receipt, not installed by this tool.
This guards accidental/corrupt archives, not hostile same-user filesystem races.
"""
import argparse
import copy
import gzip
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import tarfile
import tempfile
import time
from urllib.parse import urlsplit
from urllib.request import HTTPRedirectHandler, build_opener

ROOT = Path(__file__).resolve().parents[1]
MAX_ARCHIVE = 64 << 20
MAX_FILE = 32 << 20
MAX_TOTAL = 512 << 20
MAX_TAR_BYTES = 576 << 20  # Includes hidden PAX/GNU metadata and padding.
MAX_MEMBERS = 50000
MAX_RECEIPT = 16 << 20


def canonical(value):
    return (json.dumps(value, sort_keys=True, separators=(',', ':')) + '\n').encode()


def digest(path):
    value = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1 << 20), b''):
            value.update(block)
    return value.hexdigest()


def relative(value):
    if (not isinstance(value, str) or not value or len(value) > 4096 or
            '\\' in value or any(ord(c) < 32 for c in value)):
        raise ValueError('Unsafe relative path')
    parts = value.split('/')
    if any(p in ('', '.', '..') or len(p) > 255 or ':' in p for p in parts):
        raise ValueError('Unsafe relative path')
    return parts


def hash_value(value):
    if not isinstance(value, str) or not re.fullmatch('[0-9a-f]{64}', value):
        raise ValueError('Invalid SHA256 pin')
    return value


def https_url(value):
    if not isinstance(value, str):
        raise ValueError('Invalid download URL')
    url = urlsplit(value)
    if (url.scheme != 'https' or not url.hostname or url.username is not None or
            url.password is not None or url.fragment):
        raise ValueError('Only credential-free HTTPS downloads are permitted')
    return value


class SafeRedirect(HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, message, headers, newurl):
        https_url(newurl)
        return super().redirect_request(request, fp, code, message, headers, newurl)


def urlopen_checked(url):
    return build_opener(SafeRedirect()).open(https_url(url), timeout=30)


def directory(path):
    path = Path(os.path.abspath(path))
    # Do not follow a symlinked cache/output ancestor, even for reads/reuse.
    for current in (*reversed(path.parents), path):
        if current.is_symlink():
            raise ValueError('Symlink directory refused: ' + str(current))
        if current.exists():
            if not current.is_dir():
                raise ValueError('Directory required: ' + str(current))
        else:
            current.mkdir(mode=0o700)
    return path


def regular(path):
    info = path.lstat()
    if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
        raise ValueError('Regular single-link file required: ' + str(path))
    return info


def checked_file(path, size, expected):
    info = regular(path)
    if info.st_size != size or digest(path) != expected:
        raise ValueError('Cached file identity mismatch: ' + path.name)
    return path


def download(url, target, size, expected):
    """Bounded verified download; never replace a bad existing cache entry."""
    https_url(url)
    hash_value(expected)
    if type(size) is not int or not 0 < size <= MAX_ARCHIVE:
        raise ValueError('Invalid download size')
    if target.exists() or target.is_symlink():
        return checked_file(target, size, expected)
    started = time.monotonic()
    partial = None
    try:
        with tempfile.NamedTemporaryFile(prefix='.dxvk-download-', dir=target.parent,
                                         delete=False) as output:
            partial = Path(output.name)
            sha = hashlib.sha256()
            total = 0
            with urlopen_checked(url) as response:
                https_url(response.geturl())
                length = response.headers.get('Content-Length')
                if length is not None and int(length) != size:
                    raise ValueError('Download Content-Length differs from pin')
                while True:
                    block = response.read(min(1 << 20, size - total + 1))
                    if not block:
                        break
                    total += len(block)
                    if total > size or time.monotonic() - started > 300:
                        raise ValueError('Download exceeds size/time bound')
                    sha.update(block)
                    output.write(block)
            if total != size or sha.hexdigest() != expected:
                raise ValueError('Downloaded file identity mismatch')
            output.flush()
            os.fsync(output.fileno())
        # Another cooperative acquisition may have published the same input.
        if target.exists() or target.is_symlink():
            return checked_file(target, size, expected)
        os.replace(partial, target)
        partial = None
        return target
    finally:
        if partial is not None:
            partial.unlink(missing_ok=True)


def files(path):
    if path.is_symlink() or not path.is_dir():
        raise ValueError('Source directory required without symlinks')
    result = []
    count = total = 0
    # Avoid materializing an unbounded rglob list before applying limits.
    for item in path.rglob('*'):
        count += 1
        if count > MAX_MEMBERS:
            raise ValueError('Source entry count exceeded')
        if item.is_symlink():
            raise ValueError('Source symlink refused')
        if item.is_file():
            info = regular(item)
            total += info.st_size
            if info.st_size > MAX_FILE or total > MAX_TOTAL:
                raise ValueError('Source file/tree byte limit exceeded')
            result.append({'path': item.relative_to(path).as_posix(),
                           'bytes': info.st_size, 'sha256': digest(item),
                           'mode': stat.S_IMODE(info.st_mode)})
        elif not item.is_dir():
            raise ValueError('Unexpected source entry type')
    return sorted(result, key=lambda row: row['path'])


def tree_hash(entries):
    return hashlib.sha256(canonical({row['path']: row['sha256'] for row in entries})).hexdigest()


class LimitedTarStream:
    """Bound decompression before tarfile can consume hidden extended headers."""
    def __init__(self, stream, budget):
        self.stream = stream
        self.budget = budget

    def read(self, size=-1):
        remaining = MAX_TAR_BYTES - self.budget.get('tarBytes', 0)
        requested = remaining + 1 if size < 0 else min(size, remaining + 1)
        data = self.stream.read(requested)
        if len(data) > remaining:
            raise ValueError('Decompressed tar stream limit exceeded')
        self.budget['tarBytes'] = self.budget.get('tarBytes', 0) + len(data)
        return data


def extract(archive, target, budget):
    if target.exists():
        if target.is_symlink() or not target.is_dir() or any(target.iterdir()):
            raise ValueError('Refuse non-empty extraction target')
    else:
        target.mkdir(parents=True)
    seen = set()
    root = None
    with gzip.open(archive, 'rb') as compressed, \
            tarfile.open(fileobj=LimitedTarStream(compressed, budget), mode='r|') as source:
        for member in source:
            # Only trailing separators on directory records are tolerated.
            name = member.name.rstrip('/') if member.isdir() else member.name
            parts = relative(name)
            if root is None:
                root = parts[0]
            if parts[0] != root or name in seen:
                raise ValueError('Duplicate/multiple archive roots')
            seen.add(name)
            budget['members'] += 1
            if budget['members'] > MAX_MEMBERS:
                raise ValueError('Archive member count exceeded')
            if member.type not in (tarfile.REGTYPE, tarfile.AREGTYPE, tarfile.DIRTYPE) or member.sparse:
                raise ValueError('Archive links/devices/sparse entries are forbidden')
            if len(parts) == 1:
                if not member.isdir():
                    raise ValueError('Archive root must be a directory')
                continue
            destination = target.joinpath(*parts[1:])
            if member.isdir():
                destination.mkdir(parents=True, exist_ok=True)
                continue
            budget['bytes'] += member.size
            if member.size < 0 or member.size > MAX_FILE or budget['bytes'] > MAX_TOTAL:
                raise ValueError('Unpacked archive size exceeded')
            destination.parent.mkdir(parents=True, exist_ok=True)
            with source.extractfile(member) as input_file, destination.open('xb') as output:
                remaining = member.size
                while remaining:
                    block = input_file.read(min(remaining, 1 << 20))
                    if not block:
                        raise ValueError('Truncated tar member')
                    output.write(block)
                    remaining -= len(block)
            destination.chmod(0o755 if member.mode & 0o111 else 0o644)
    if not seen:
        raise ValueError('Empty source archive')


def validate_lock(lock):
    if lock.get('schemaVersion') != 2:
        raise ValueError('DXVK source acquisition requires schemaVersion 2')
    inputs = lock['buildInputs']
    archives = inputs['archives']
    if not isinstance(archives, list) or not 2 <= len(archives) <= 16:
        raise ValueError('Invalid source archive set')
    names, destinations, cache_names = set(), set(), set()
    for row in archives:
        for field, collection in (('name', names), ('path', destinations), ('archiveFile', cache_names)):
            value = row[field]
            relative(value)
            if value in collection:
                raise ValueError('Duplicate source identity/destination')
            collection.add(value)
        if len(relative(row['archiveFile'])) != 1:
            raise ValueError('Archive cache filename must be a basename')
        if not re.fullmatch('[0-9a-f]{40}', row['commit']):
            raise ValueError('Full source commit required')
        https_url(row['archiveUrl'])
        hash_value(row['archiveSha256'])
        if type(row['archiveBytes']) is not int or not 0 < row['archiveBytes'] <= MAX_ARCHIVE:
            raise ValueError('Invalid archive byte limit')
    bases = {row['path']: row for row in archives if row.get('base') is True}
    if set(bases) != {'legacy', 'modern'} or any(row['name'] != name for name, row in bases.items()):
        raise ValueError('Both exact fork roots are required')
    for row in archives:
        parts = relative(row['path'])
        if parts[0] not in bases or (len(parts) == 1 and not row.get('base')):
            raise ValueError('Submodule outside pinned fork root')
    # Nested dependency destinations would permit one archive to overwrite another.
    nested = [row['path'] for row in archives if not row.get('base')]
    if any(a != b and b.startswith(a + '/') for a in nested for b in nested):
        raise ValueError('Overlapping submodule roots')
    for variant, name in (('legacy-1.10.3', 'legacy'), ('modern-2.6.2', 'modern')):
        source = lock['variants'][variant]['source']
        row = bases[name]
        if source['commit'] != row['commit'] or source['archiveFile'] != row['archiveFile']:
            raise ValueError('Variant/archive source identity differs')
        hash_value(source['materializedTreeSha256'])
    bcdec = inputs['bcdec']
    relative(bcdec['path'])
    if (bcdec['path'] != 'dlls/d3dx9_36/bcdec.h' or
            bcdec['stagedPath'] != 'source/wine/' + bcdec['path'] or
            not re.fullmatch('[0-9a-f]{40}', bcdec['commit'])):
        raise ValueError('Unexpected BC decoder source/path')
    if type(bcdec['bytes']) is not int or not 0 < bcdec['bytes'] <= 1 << 20:
        raise ValueError('Invalid BC decoder length')
    hash_value(bcdec['sha256'])
    return bases


def verify_tree(lock, output):
    if output.is_symlink() or not output.is_dir():
        raise ValueError('Source root must be a non-symlink directory')
    if {p.name for p in output.iterdir()} - {'legacy', 'modern', 'wine', 'source-inputs.json'}:
        raise ValueError('Unexpected source-root contents')
    trees = {}
    for variant, name in (('legacy-1.10.3', 'legacy'), ('modern-2.6.2', 'modern')):
        entries = files(output/name)
        value = tree_hash(entries)
        if value != lock['variants'][variant]['source']['materializedTreeSha256']:
            raise ValueError('Materialized fork tree mismatch: ' + name)
        trees[name] = {'files': entries, 'treeSha256': value}
    bcdec = lock['buildInputs']['bcdec']
    entries = files(output/'wine')
    if len(entries) != 1 or entries[0]['path'] != bcdec['path']:
        raise ValueError('Unexpected Wine header tree')
    checked_file(output/'wine'/bcdec['path'], bcdec['bytes'], bcdec['sha256'])
    return trees


def acquisition_spec(lock):
    # Build result hashes/runtime policy can change without changing any source.
    # Preserve the initial whole-lock hash for audit, but key source reuse by
    # complete source/tool input provenance rather than unrelated output fields.
    return {'buildInputs': lock['buildInputs'],
            'sources': {key: row['source'] for key, row in lock['variants'].items()}}


def acquire(lock, cache, output):
    validate_lock(lock)
    cache = directory(cache)
    output = Path(os.path.abspath(output))
    directory(output.parent)
    lock_sha = hashlib.sha256(canonical(lock)).hexdigest()
    spec_sha = hashlib.sha256(canonical(acquisition_spec(lock))).hexdigest()
    if output.exists() or output.is_symlink():
        trees = verify_tree(lock, output)
        receipt = output/'source-inputs.json'
        if regular(receipt).st_size > MAX_RECEIPT:
            raise ValueError('Oversized acquisition receipt')
        prior = json.loads(receipt.read_text())
        if prior.get('acquisitionSpecSha256') != spec_sha or prior.get('sourceTrees') != trees:
            raise ValueError('Existing source receipt differs; choose a fresh output')
        return prior
    with tempfile.TemporaryDirectory(prefix='.' + output.name + '.prepare-', dir=output.parent) as temporary:
        stage = Path(temporary)
        source_inputs = []
        budget = {'members': 0, 'bytes': 0}
        for row in sorted(lock['buildInputs']['archives'], key=lambda r: (len(relative(r['path'])), r['path'])):
            archive = download(row['archiveUrl'], cache/row['archiveFile'], row['archiveBytes'], row['archiveSha256'])
            target = stage/row['path']
            extract(archive, target, budget)
            entries = files(target)
            source_inputs.append({**copy.deepcopy(row), 'files': entries, 'treeSha256': tree_hash(entries)})
        bcdec = lock['buildInputs']['bcdec']
        url = ('https://raw.githubusercontent.com/winehua/wine/' + bcdec['commit'] + '/' + bcdec['path'])
        header = download(url, cache/('bcdec-' + bcdec['commit'] + '.h'), bcdec['bytes'], bcdec['sha256'])
        target = stage/'wine'/bcdec['path']
        target.parent.mkdir(parents=True)
        shutil.copyfile(header, target)
        target.chmod(0o644)
        trees = verify_tree(lock, stage)
        receipt = {'schema': 1, 'lockSchema': 2, 'lockSha256': lock_sha,
                   'acquisitionSpecSha256': spec_sha,
                   'sourceInputs': source_inputs, 'sourceTrees': trees,
                   'bcdec': {**copy.deepcopy(bcdec), 'url': url},
                   'toolInputs': {key: copy.deepcopy(lock['buildInputs'].get(key)) for key in ('llvmMingw', 'hostPackages', 'hostBinaries')},
                   'treeHashAlgorithm': 'SHA256(json.dumps({path:sha256},sort_keys=True,separators=(comma,colon))+newline)',
                   'sourceFilesModified': False, 'runtimeExecuted': False}
        (stage/'source-inputs.json').write_bytes(canonical(receipt))
        if output.exists() or output.is_symlink():
            raise ValueError('Output appeared during acquisition; refusing replacement')
        os.rename(stage, output)
    return receipt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--lock', type=Path, default=ROOT/'docs/linux-steam/gnu-dxvk-source.lock.json')
    parser.add_argument('--cache', type=Path, default=ROOT/'build/gnu-dxvk/archives')
    parser.add_argument('--output', type=Path, required=True, help='Fresh source root; never rewrites existing build sources')
    args = parser.parse_args()
    if regular(args.lock).st_size > MAX_RECEIPT:
        parser.error('Oversized lock')
    result = acquire(json.loads(args.lock.read_text()), args.cache, args.output)
    print(json.dumps({'output': str(args.output), 'lockSha256AtCreation': result['lockSha256'],
                      'acquisitionSpecSha256': result['acquisitionSpecSha256'],
                      'trees': {name: row['treeSha256'] for name, row in result['sourceTrees'].items()}}))


if __name__ == '__main__':
    main()
