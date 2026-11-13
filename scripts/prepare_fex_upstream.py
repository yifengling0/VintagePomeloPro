#!/usr/bin/env python3
"""Prepare/verify the isolated, locked FEX migration without changing production.

Full source and dependency blobs are checked against Git trees. Archives stream
to their fixed destination; no duplicate tarballs or recursive vendor copies.
"""
import argparse
import configparser
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import subprocess
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
LOCK = ROOT / 'docs/upstream-sync/fex-candidate.lock.json'


def sha(data):
    return hashlib.sha256(data).hexdigest()


def git(source, *args, data=None):
    return subprocess.check_output(['git', '-c', 'gc.auto=0', '-C', str(source), *args], input=data)


def json_cache(path, url):
    if not path.exists():
        with urllib.request.urlopen(url, timeout=45) as stream:
            value = json.load(stream)
        if value.get('truncated'):
            raise ValueError('GitHub returned an incomplete tree')
        path.write_text(json.dumps(value) + '\n')
    value = json.loads(path.read_text())
    if value.get('truncated'):
        raise ValueError('Cached Git tree is incomplete')
    return value


def blob(data):
    return hashlib.sha1(b'blob ' + str(len(data)).encode() + b'\0' + data).hexdigest()


def verify_tree(tree, expected):
    """Check the Git tree object, not the endpoint's echoed commit query ID."""
    if tree.get('truncated'):
        raise ValueError('Git tree response is incomplete')
    root = {}
    for row in tree['tree']:
        if row['type'] == 'tree':
            continue
        name = PurePosixPath(row['path'])
        if name.is_absolute() or '..' in name.parts or '\\' in row['path']:
            raise ValueError('Unsafe Git tree path')
        node = root
        for part in name.parts[:-1]:
            node = node.setdefault(part, {})
        if name.name in node:
            raise ValueError('Duplicate Git tree path')
        node[name.name] = (row['mode'].lstrip('0'), row['sha'])

    def digest(node):
        items = []
        for name, value in node.items():
            if isinstance(value, dict):
                mode, oid, key = '40000', digest(value), name.encode() + b'/'
            else:
                (mode, oid), key = value, name.encode()
            items.append((key, mode.encode() + b' ' + name.encode() + b'\0' + bytes.fromhex(oid)))
        data = b''.join(value for _, value in sorted(items))
        return hashlib.sha1(b'tree ' + str(len(data)).encode() + b'\0' + data).hexdigest()

    actual = digest(root)
    if actual != expected:
        raise ValueError('Reconstructed Git tree differs from lock: ' + actual)
    return actual


def verify_files(target, entries):
    for name, item in entries.items():
        path = target / name
        data = os.fsencode(os.readlink(path)) if item['mode'] == '120000' else path.read_bytes()
        if blob(data) != item['sha']:
            raise ValueError('FEX source blob mismatch: ' + str(path))


def materialize(repo, pin, target, tree):
    files = {row['path']: row for row in tree['tree'] if row['type'] == 'blob'}
    # A matching tree is reusable. A partial directory is repaired in place
    # from the same locked archive, without creating a second extraction tree.
    try:
        verify_files(target, files)
        return
    except (FileNotFoundError, ValueError):
        pass
    seen = set()
    print('Preparing', repo, pin, 'bytes', sum(row.get('size', 0) for row in files.values()), flush=True)
    with urllib.request.urlopen(f'https://api.github.com/repos/{repo}/tarball/{pin}', timeout=45) as stream:
        with tarfile.open(fileobj=stream, mode='r|gz') as archive:
            for member in archive:
                path = PurePosixPath(member.name)
                if path.is_absolute() or '..' in path.parts or '\\' in member.name:
                    raise ValueError('Unsafe archive path')
                if member.isdir():
                    continue
                name = PurePosixPath(*path.parts[1:]).as_posix()
                if name not in files or name in seen or not (member.isfile() or member.issym()):
                    raise ValueError('Archive inventory differs from locked tree: ' + name)
                row = files[name]
                data = member.linkname.encode() if member.issym() else archive.extractfile(member).read()
                if blob(data) != row['sha']:
                    raise ValueError('Archive blob checksum mismatch: ' + name)
                dest = target / name
                dest.parent.mkdir(parents=True, exist_ok=True)
                if row['mode'] == '120000':
                    if not dest.is_symlink():
                        dest.symlink_to(data.decode())
                    elif os.readlink(dest) != data.decode():
                        raise ValueError('Existing source symlink has changed')
                else:
                    if dest.is_symlink():
                        raise ValueError('Refusing to write through a source symlink')
                    dest.write_bytes(data)
                    dest.chmod(0o755 if row['mode'] == '100755' else 0o644)
                seen.add(name)
    if seen != set(files):
        raise ValueError('Archive omitted locked source files')


def replay_history(source, lock):
    """Restore exact OHOS commits, including their real upstream parent."""
    history = json.loads((ROOT / 'docs/upstream-sync/fex-adapted-history.json').read_text())
    parent = lock['upstream']['commit']
    for row in lock['patches']:
        patch = ROOT / row['path']
        if sha(patch.read_bytes()) != row['sha256']:
            raise ValueError('Candidate patch changed: ' + row['path'])
        git(source, 'apply', '--index', '--whitespace=nowarn', str(patch))
        raw = history[row['commit']].encode()
        headers = raw.split(b'\n\n', 1)[0].splitlines()
        parents = [line[7:].decode() for line in headers if line.startswith(b'parent ')]
        trees = [line[5:].decode() for line in headers if line.startswith(b'tree ')]
        if parents != [parent] or trees != [git(source, 'write-tree').decode().strip()]:
            raise ValueError('Adapted commit history does not match patch replay')
        actual = git(source, 'hash-object', '-w', '-t', 'commit', '--stdin', data=raw).decode().strip()
        if actual != row['commit']:
            raise ValueError('Adapted commit object differs from lock')
        parent = actual
    if parent != lock['adaptedCommit']:
        raise ValueError('Adapted history does not end at locked commit')
    git(source, 'update-ref', 'refs/heads/ohos/candidate', parent, lock['upstream']['commit'])


def verify_source(source, lock):
    if git(source, 'rev-parse', 'HEAD').decode().strip() != lock['adaptedCommit']:
        raise ValueError('FEX HEAD differs from the locked adapted commit')
    actual = git(source, 'write-tree').decode().strip()
    if actual != lock['adaptedTree']:
        raise ValueError('FEX index differs from the locked adapted tree')
    entries = {}
    for line in git(source, 'ls-files', '--stage', '-z').split(b'\0'):
        if not line:
            continue
        meta, path = line.split(b'\t', 1)
        mode, oid, stage = meta.decode().split()
        if stage != '0':
            raise ValueError('Unresolved FEX merge')
        if mode != '160000':
            entries[path.decode()] = {'mode': mode, 'sha': oid}
    verify_files(source, entries)
    if git(source, 'ls-files', '--others', '--exclude-standard').strip():
        raise ValueError('Unregistered files in candidate source')
    return len(entries)


def prepare(source, cache, reuse, verify_only):
    lock = json.loads(LOCK.read_text())
    for row in lock['patches']:
        if sha((ROOT / row['path']).read_bytes()) != row['sha256']:
            raise ValueError('Candidate patch changed: ' + row['path'])
    upstream = lock['upstream']
    cache.mkdir(parents=True, exist_ok=True)
    if not (source / '.git').exists():
        if verify_only:
            raise ValueError('Candidate has not been prepared')
        if source.exists() and any(source.iterdir()):
            raise ValueError('Refusing to adopt an existing non-Git source tree')
        source.mkdir(parents=True, exist_ok=True)
        tree = json_cache(cache / ('fex-tree-' + upstream['commit'] + '.json'),
                          f'https://api.github.com/repos/FEX-Emu/FEX/git/trees/{upstream["commit"]}?recursive=1')
        verify_tree(tree, upstream['tree'])
        materialize('FEX-Emu/FEX', upstream['commit'], source, tree)
        subprocess.run(['git', 'init', '-q', str(source)], check=True)
        commit = (ROOT / 'docs/upstream-sync/fex-upstream.commit').read_bytes()
        if git(source, 'hash-object', '-w', '-t', 'commit', '--stdin', data=commit).decode().strip() != upstream['commit']:
            raise ValueError('Upstream commit object differs from lock')
        entries = []
        for row in tree['tree']:
            if row['type'] == 'tree':
                continue
            path = source / row['path']
            if row['type'] == 'blob':
                contents = os.fsencode(os.readlink(path)) if row['mode'] == '120000' else path.read_bytes()
                if git(source, 'hash-object', '-w', '--stdin', data=contents).decode().strip() != row['sha']:
                    raise ValueError('Unexpected staged source blob')
            else:
                path.mkdir(parents=True, exist_ok=True)
            entries.append(f'{row["mode"]} {row["sha"]}\t{row["path"]}\0'.encode())
        git(source, 'update-index', '-z', '--index-info', data=b''.join(entries))
        if git(source, 'write-tree').decode().strip() != upstream['tree']:
            raise ValueError('Reconstructed upstream tree differs')
        (source / '.git/shallow').write_text(upstream['commit'] + '\n')
        git(source, 'update-ref', 'refs/heads/ohos/candidate', upstream['commit'])
        git(source, 'symbolic-ref', 'HEAD', 'refs/heads/ohos/candidate')
        replay_history(source, lock)
    count = verify_source(source, lock)

    config = configparser.ConfigParser()
    config.read(source / '.gitmodules')
    urls = {config[s]['path']: config[s]['url'].removesuffix('.git') for s in config.sections()}
    dependencies = []
    for row in lock['dependencies']:
        name, pin = row['path'], row['commit']
        if row.get('state') == 'not-used-BUILD_TESTING-off':
            continue
        target = source / name
        existing = reuse / name if reuse else None
        if existing and (existing / '.git').exists() and git(existing, 'rev-parse', 'HEAD').decode().strip() == pin:
            if git(existing, 'diff', 'HEAD', '--').strip():
                raise ValueError('Reusable dependency has edits: ' + name)
            records = []
            for entry in git(existing, 'ls-files', '--stage', '-z').split(b'\0'):
                if not entry:
                    continue
                meta, path = entry.split(b'\t', 1)
                mode, oid, stage = meta.decode().split()
                if mode != '160000':
                    records.append((path.decode(), {'mode': mode, 'sha': oid}))
            if not verify_only:
                target.mkdir(parents=True, exist_ok=True)
                for child in existing.iterdir():
                    dest = target / child.name
                    if child.name == '.git' or dest.exists() or dest.is_symlink():
                        continue
                    dest.symlink_to(os.path.relpath(child, target), target_is_directory=child.is_dir())
            verify_files(target, dict(records))
            dependencies.append({'path': name, 'commit': pin, 'verifiedFiles': len(records), 'reused': True})
        else:
            repo = urls[name].split('github.com/')[1]
            tree = json_cache(cache / (name.replace('/', '-') + '-' + pin + '-tree.json'),
                              f'https://api.github.com/repos/{repo}/git/trees/{pin}?recursive=1')
            actual_tree = verify_tree(tree, row['tree'])
            files = {x['path']: x for x in tree['tree'] if x['type'] == 'blob'}
            if verify_only:
                verify_files(target, files)
            else:
                materialize(repo, pin, target, tree)
            dependencies.append({'path': name, 'commit': pin, 'tree': actual_tree, 'verifiedFiles': len(files), 'reused': False})
    result = {'lockSha256': sha(LOCK.read_bytes()), 'upstreamCommit': upstream['commit'],
              'adaptedTree': lock['adaptedTree'], 'verifiedFiles': count,
              'dependencies': dependencies, 'productionPinChanged': False}
    (cache / 'fex-preparation.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({'verifiedFiles': count, 'dependencies': len(dependencies), 'tree': lock['adaptedTree']}))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--cache', type=Path, required=True)
    parser.add_argument('--reuse', type=Path)
    parser.add_argument('--verify-only', action='store_true')
    args = parser.parse_args()
    prepare(args.source.resolve(), args.cache.resolve(), args.reuse.resolve() if args.reuse else None, args.verify_only)
