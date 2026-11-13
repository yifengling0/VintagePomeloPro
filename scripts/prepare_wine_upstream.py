#!/usr/bin/env python3
"""Prepare a locked complete Valve Wine tree, reusing the OHOS object database.

Only missing blobs are stored. The complete adapted tree and exact commit
history are replayed in isolation; production sources and pins stay read only.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import subprocess
import tarfile
import urllib.request

from prepare_fex_upstream import blob, json_cache, verify_tree

ROOT = Path(__file__).resolve().parents[1]
LOCK = ROOT / 'docs/upstream-sync/wine-candidate.lock.json'


def git(repo, *args, data=None, env=None):
    p = subprocess.run(['git', '-c', 'gc.auto=0', '-C', str(repo), *args], input=data,
                       capture_output=True, timeout=180, env=env)
    if p.returncode:
        raise ValueError('git ' + ' '.join(args[:2]) + ': ' + p.stderr.decode(errors='replace')[-2500:])
    return p.stdout


def replay_history(repo, lock, cache, root=ROOT):
    """Replay into a separate index, without copying or modifying any worktree."""
    history_path = root / lock['history']['path']
    if hashlib.sha256(history_path.read_bytes()).hexdigest() != lock['history']['sha256']:
        raise ValueError('Wine commit history checksum differs from lock')
    history = json.loads(history_path.read_text())
    index = cache / 'wine-replay.index'
    if index.exists():
        raise ValueError('Wine replay index already exists; check for another active preparation')
    env = dict(os.environ, GIT_INDEX_FILE=str(index))
    parent = lock['upstream']['commit']
    try:
        git(repo, 'read-tree', parent, env=env)
        for row in lock['patches']:
            patch = root / row['path']
            if hashlib.sha256(patch.read_bytes()).hexdigest() != row['sha256']:
                raise ValueError('Wine candidate patch changed: ' + row['path'])
            git(repo, 'apply', '--cached', '--whitespace=nowarn', str(patch), env=env)
            raw = history[row['commit']].encode()
            headers = raw.split(b'\n\n', 1)[0].splitlines()
            parents = [line[7:].decode() for line in headers if line.startswith(b'parent ')]
            trees = [line[5:].decode() for line in headers if line.startswith(b'tree ')]
            actual_tree = git(repo, 'write-tree', env=env).decode().strip()
            if parents != [parent] or trees != [actual_tree]:
                raise ValueError('Wine adapted commit parent/tree differs from replay')
            parent = git(repo, 'hash-object', '-w', '-t', 'commit', '--stdin', data=raw).decode().strip()
            if parent != row['commit']:
                raise ValueError('Wine adapted commit object differs from lock')
        if parent != lock['adaptedCommit'] or actual_tree != lock['adaptedTree']:
            raise ValueError('Wine history does not end at the locked candidate')
        return parent
    finally:
        index.unlink(missing_ok=True)


def prepare(repo, source, cache):
    lock = json.loads(LOCK.read_text())
    upstream, baseline = lock['upstream'], lock['baseline']
    for path in (repo, source, cache):
        if path.is_symlink() or not path.resolve().is_relative_to(ROOT / 'workspace_temp'):
            raise ValueError('Use an isolated repository/worktree under workspace_temp')
    if git(repo, 'rev-parse', baseline['commit'] + '^{tree}').decode().strip() != baseline['tree']:
        raise ValueError('Reconstructed OHOS baseline differs from the audited tree')
    cache.mkdir(parents=True, exist_ok=True)
    treepath = cache / 'wine-tree-cache' / (upstream['commit'] + '.json')
    treepath.parent.mkdir(parents=True, exist_ok=True)
    value = json_cache(treepath, 'https://api.github.com/repos/ValveSoftware/wine/git/trees/'
                       + upstream['tree'] + '?recursive=1')
    verify_tree(value, upstream['tree'])
    rows = {r['path']: r for r in value['tree'] if r['type'] != 'tree'}
    if any(r['type'] != 'blob' for r in rows.values()):
        raise ValueError('Nested Wine dependencies require explicit handling')
    byhash = {r['sha']: name for name, r in rows.items()}
    objects = git(repo, 'cat-file', '--batch-check', data=('\n'.join(byhash) + '\n').encode())
    missing = {line.split()[0].decode() for line in objects.splitlines() if line.endswith(b' missing')}
    missing_bytes = sum(rows[byhash[oid]]['size'] for oid in missing)
    print(json.dumps(dict(files=len(rows),sourceBytes=sum(r['size'] for r in rows.values()),
                          missingBlobs=len(missing),missingBytes=missing_bytes)), flush=True)
    downloaded = set()
    if missing:
        url = 'https://api.github.com/repos/ValveSoftware/wine/tarball/' + upstream['commit']
        with urllib.request.urlopen(url, timeout=45) as stream:
            with tarfile.open(fileobj=stream, mode='r|gz') as archive:
                seen, prefix = set(), None
                for member in archive:
                    path = PurePosixPath(member.name)
                    if path.is_absolute() or '..' in path.parts or '\\' in member.name:
                        raise ValueError('Unsafe Wine archive path')
                    prefix = prefix or path.parts[0]
                    if path.parts[0] != prefix:
                        raise ValueError('Archive root changed')
                    if member.isdir():
                        continue
                    name = PurePosixPath(*path.parts[1:]).as_posix()
                    if name not in rows or name in seen or not (member.isfile() or member.issym()):
                        raise ValueError('Archive differs from complete Wine tree: ' + name)
                    row = rows[name]
                    data = member.linkname.encode() if member.issym() else archive.extractfile(member).read()
                    if len(data) != row['size'] or blob(data) != row['sha']:
                        raise ValueError('Wine archive blob mismatch: ' + name)
                    if row['sha'] in missing and row['sha'] not in downloaded:
                        oid = git(repo, 'hash-object', '-w', '--stdin', data=data).decode().strip()
                        if oid != row['sha']:
                            raise ValueError('Stored Wine object differs from archive')
                        downloaded.add(oid)
                    seen.add(name)
                if seen != set(rows) or downloaded != missing:
                    raise ValueError('Incomplete Wine source archive')
    # Reconstruct complete tree objects from the locked leaf inventory.
    with_index = dict(os.environ, GIT_INDEX_FILE=str(repo / 'upstream-index'))
    p = subprocess.run(['git', '-c', 'gc.auto=0', '-C', str(repo), 'read-tree', '--empty'],
                       env=with_index, capture_output=True, check=True)
    records = b''.join(f'{r["mode"]} {r["sha"]}\t{name}\0'.encode() for name,r in sorted(rows.items()))
    subprocess.run(['git', '-c', 'gc.auto=0', '-C', str(repo), 'update-index', '-z', '--index-info'],
                   input=records, env=with_index, capture_output=True, check=True)
    actual = subprocess.check_output(['git', '-C', str(repo), 'write-tree'], env=with_index).decode().strip()
    if actual != upstream['tree']:
        raise ValueError('Stored complete Wine tree differs from upstream')
    raw = (ROOT / 'docs/upstream-sync/wine-upstream.commit').read_bytes()
    if git(repo, 'hash-object', '-w', '-t', 'commit', '--stdin', data=raw).decode().strip() != upstream['commit']:
        raise ValueError('Official Wine commit object identity mismatch')
    shallow = repo / 'shallow'
    boundaries = set(shallow.read_text().splitlines()) if shallow.exists() else set()
    boundaries.add(upstream['commit'])
    shallow.write_text('\n'.join(sorted(boundaries)) + '\n')
    git(repo, 'update-ref', 'refs/heads/valve/experimental-locked', upstream['commit'])
    adapted = replay_history(repo, lock, cache)
    git(repo, 'update-ref', 'refs/heads/ohos/experimental-locked', adapted)
    if not (source / '.git').exists():
        if source.exists() and any(source.iterdir()):
            raise ValueError('Refusing to adopt nonempty candidate directory')
        git(repo, 'worktree', 'add', '--detach', '--no-checkout', str(source), adapted)
        git(repo, 'config', 'extensions.worktreeConfig', 'true')
        git(source, 'config', '--worktree', 'core.bare', 'false')
        git(source, 'config', '--worktree', 'core.sparseCheckout', 'false')
        git(source, 'read-tree', '-mu', 'HEAD')
    if git(source, 'rev-parse', 'HEAD').decode().strip() != adapted:
        raise ValueError('Existing Wine worktree differs from locked candidate; preserve local work')
    if git(source, 'status', '--porcelain', '--untracked-files=all').strip():
        raise ValueError('Wine worktree contains unregistered changes')
    if git(source, 'write-tree').decode().strip() != lock['adaptedTree']:
        raise ValueError('Wine worktree index differs from locked candidate')
    result = dict(schemaVersion=1,upstreamCommit=upstream['commit'],upstreamTree=actual,
                  completeFiles=len(rows),downloadedBlobs=len(missing),downloadedBytes=missing_bytes,
                  allSourceObjectsAvailable=True,sourceCheckout='locked adapted worktree',
                  adaptedCommit=adapted,adaptedTree=lock['adaptedTree'],
                  patchesReplayed=len(lock['patches']),exactHistoryVerified=True,
                  baselineCommit=baseline['commit'],productionPinChanged=False)
    (cache / 'wine-preparation.json').write_text(json.dumps(result,indent=2) + '\n')
    print(json.dumps(result), flush=True)


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--maintenance', type=Path, required=True)
    p.add_argument('--source', type=Path, required=True)
    p.add_argument('--cache', type=Path, required=True)
    args = p.parse_args()
    prepare(args.maintenance.resolve(), args.source.resolve(), args.cache.resolve())
