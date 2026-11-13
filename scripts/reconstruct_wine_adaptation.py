#!/usr/bin/env python3
"""Rebuild the current OHOS adaptation on its real Valve parent without a full copy.

Uses an isolated bare object database, temporary index and sparse overlay files.
The product source, its Git index, production gitlink and build caches are read only.
The resulting maintenance branch is an equivalence baseline, not an upgraded Wine.
"""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
from concurrent.futures import ThreadPoolExecutor

from verify_wine_overlay_stack import paths
from wine_build_identity import ordered_overlays
import wine_runtime_diagnostics
from verify_wine_vulkan_generation import verify as verify_vulkan_generation

ROOT = Path(__file__).resolve().parents[1]


def git(repo, *args, data=None, env=None):
    result = subprocess.run(['git', '-c', 'gc.auto=0', '-C', str(repo), *args],
                            input=data, capture_output=True, env=env, timeout=180)
    if result.returncode:
        raise ValueError('git ' + ' '.join(args[:2]) + ': ' + result.stderr.decode(errors='replace')[-2000:])
    return result.stdout


def tree_entries(repo, ref):
    result = {}
    for record in git(repo, 'ls-tree', '-rz', ref).split(b'\0'):
        if not record:
            continue
        meta, name = record.split(b'\t', 1)
        mode, _, sha = meta.decode().split()
        result[os.fsdecode(name)] = (mode, sha)
    return result


def update_index(repo, changes, env):
    data = b''.join(mode.encode() + b' ' + sha.encode() + b'\t' + os.fsencode(name) + b'\0'
                    for name, (mode, sha) in sorted(changes.items()))
    git(repo, 'update-index', '-z', '--index-info', data=data, env=env)


def set_files(repo, directory, names, current, env):
    changes = {}
    for name in sorted(names):
        p = directory / name
        if p.is_symlink():
            mode, data = '120000', os.fsencode(os.readlink(p))
        elif p.is_file():
            mode = '100755' if p.stat().st_mode & 0o111 else '100644'
            data = p.read_bytes()
        elif not p.exists():
            if name in current:
                changes[name] = ('0', '0' * 40)
                del current[name]
            continue
        else:
            raise ValueError('Unsupported source file: ' + name)
        sha = git(repo, 'hash-object', '-w', '--stdin', data=data).decode().strip()
        if current.get(name) != (mode, sha):
            changes[name] = (mode, sha)
            current[name] = (mode, sha)
    update_index(repo, changes, env)
    return sorted(changes)


def theme(name):
    if name.startswith(('dlls/ntdll/', 'server/')):
        return 'process-memory-and-broker'
    if name.startswith(('dlls/winewayland.drv/', 'dlls/winex11.drv/', 'dlls/win32u/',
                        'dlls/winevulkan/', 'dlls/wined3d/', 'dlls/opengl32/')):
        return 'graphics-input-and-presentation'
    if name.startswith(('dlls/wineohos.drv/', 'dlls/winegstreamer/', 'dlls/mf', 'dlls/quartz/')):
        return 'audio-and-media'
    if name.startswith(('dlls/iphlpapi/', 'dlls/nsiproxy.sys/', 'dlls/dnsapi/', 'dlls/winhttp/', 'dlls/ws2_32/')):
        return 'network-and-platform-services'
    if name.startswith(('programs/', 'loader/')):
        return 'launcher-and-prefix'
    return 'build-abi-and-platform-integration'


def restore_valve_baseline(repo, assessment):
    """Restore hash-verified Git objects; fetch only missing original blobs.

    The audited inverse inventory supplies the original complete tree. This
    avoids a full Wine clone and works when Git's smart-HTTP proxy is unavailable.
    """
    fork = assessment['currentFork']['commit']
    base = assessment['originalValveBase']['commit']
    original = tree_entries(repo, fork)
    for item in assessment['localChanges']:
        if item['original'] is None:
            original.pop(item['path'], None)
        else:
            original[item['path']] = (item['original']['mode'], item['original']['sha'])
    nodes = {}
    for name, entry in original.items():
        current = nodes
        parts = name.split('/')
        for part in parts[:-1]:
            current = current.setdefault(part, {})
        current[parts[-1]] = entry

    def write_tree(node):
        entries = []
        for name, value in node.items():
            if isinstance(value, dict):
                mode, kind, sha = '040000', 'tree', write_tree(value)
            else:
                mode, sha = value
                kind = 'commit' if mode == '160000' else 'blob'
            entries.append(f'{mode} {kind} {sha}\t'.encode() + os.fsencode(name) + b'\0')
        return git(repo, 'mktree', '-z', '--missing', data=b''.join(entries)).decode().strip()

    tree = write_tree(nodes)
    if tree != assessment['originalValveBase']['tree']:
        raise ValueError('Inverse inventory does not reconstruct the original Valve tree')
    raw = (ROOT / 'docs/upstream-sync/wine-valve-base.commit').read_bytes()
    if git(repo, 'hash-object', '-w', '-t', 'commit', '--stdin', data=raw).decode().strip() != base:
        raise ValueError('Original Valve commit object hash mismatch')
    # This is the actual upstream commit, not a synthetic parent with its tree.
    # Retain it as a shallow boundary; deeper history can later be fetched normally.
    (repo / 'shallow').write_text(base + '\n')
    blobs = {sha: name for name, (mode, sha) in original.items() if mode != '160000'}
    response = git(repo, 'cat-file', '--batch-check', data=('\n'.join(blobs) + '\n').encode()).decode().splitlines()
    missing = [line.split()[0] for line in response if line.endswith(' missing')]

    def fetch_blob(sha):
        # GitHub's authenticated API is also usable on hosts where the Git and
        # raw-content endpoints are unreachable. gh keeps credentials private.
        gh_binary = shutil.which('gh') or str(Path.home() / '.local/bin/gh')
        response = subprocess.check_output([gh_binary, 'api',
            'repos/ValveSoftware/wine/git/blobs/' + sha], timeout=60)
        item = json.loads(response)
        if item['sha'] != sha or item['encoding'] != 'base64':
            raise ValueError('Unexpected GitHub source blob response')
        data = base64.b64decode(item['content'])
        if len(data) > 16 * 1024 * 1024:
            raise ValueError('Oversized Valve source blob')
        actual = git(repo, 'hash-object', '-w', '--stdin', data=data).decode().strip()
        if actual != sha:
            raise ValueError('Valve source blob identity mismatch: ' + blobs[sha])
        return len(data)

    with ThreadPoolExecutor(max_workers=4) as pool:
        downloaded = sum(pool.map(fetch_blob, missing))
    print(f'Valve baseline restored: {len(missing)} missing blobs, {downloaded} bytes downloaded', flush=True)


def reconstruct(source, work, report_path):
    source, work = source.resolve(), work.resolve()
    if not work.is_relative_to(ROOT / 'workspace_temp'):
        raise ValueError('Use a managed directory under this repository workspace_temp')
    assessment = json.loads((ROOT / 'docs/upstream-sync/initial-assessment.json').read_text())['wine']
    base, fork = (assessment['originalValveBase']['commit'], assessment['currentFork']['commit'])
    if git(source, 'rev-parse', 'HEAD').decode().strip() != fork:
        raise ValueError('Effective source does not belong to the audited Wine fork')
    work.mkdir(parents=True, exist_ok=True)
    repo = work / 'wine-adaptation.git'
    if repo.is_symlink():
        raise ValueError('Linked maintenance repository rejected')
    if not repo.exists():
        subprocess.run(['git', 'init', '--bare', '-q', str(repo)], check=True)
        common = Path(git(source, 'rev-parse', '--path-format=absolute', '--git-common-dir').decode().strip())
        (repo / 'objects/info/alternates').write_text(str(common / 'objects') + '\n')
    # Older Git versions initiate lazy fetches even while constructing trees
    # with --missing. This object-only reconstruction uses explicit HTTPS blob
    # reads and must have no promisor remote.
    if 'valve' in git(repo, 'remote').decode().splitlines():
        git(repo, 'remote', 'remove', 'valve')
    restore_valve_baseline(repo, assessment)
    if git(repo, 'rev-parse', base + '^{tree}').decode().strip() != assessment['originalValveBase']['tree']:
        raise ValueError('Fetched Valve baseline tree differs from audited source')
    original, fork_entries = tree_entries(repo, base), tree_entries(repo, fork)
    changed = sorted(n for n in set(original) | set(fork_entries) if original.get(n) != fork_entries.get(n))
    if len(changed) != len(assessment['localChanges']):
        raise ValueError('Committed adaptation inventory changed')

    # Fixed metadata makes reconstruction deterministic, independent of run time.
    date = git(source, 'show', '-s', '--format=%cI', fork).decode().strip()
    env = dict(os.environ, GIT_AUTHOR_NAME='VPP Runtime Maintainers',
               GIT_COMMITTER_NAME='VPP Runtime Maintainers', GIT_AUTHOR_EMAIL='runtime@vintagepomelo.invalid',
               GIT_COMMITTER_EMAIL='runtime@vintagepomelo.invalid', GIT_AUTHOR_DATE=date, GIT_COMMITTER_DATE=date)
    index = work / 'wine-index'
    if index.is_symlink():
        raise ValueError('Linked reconstruction index rejected')
    env['GIT_INDEX_FILE'] = str(index)
    commits, overlays = [], ordered_overlays(ROOT)
    parent, current = base, dict(original)

    def commit(label, provenance):
        nonlocal parent
        tree = git(repo, 'write-tree', env=env).decode().strip()
        if tree == git(repo, 'rev-parse', parent + '^{tree}').decode().strip():
            return None
        message = f'OHOS adaptation: {label}\n\nReconstructed from {fork}.\nValve baseline: {base}.\n{provenance}\n'
        sha = git(repo, 'commit-tree', tree, '-p', parent, data=message.encode(), env=env).decode().strip()
        commits.append({'commit': sha, 'tree': tree, 'label': label})
        parent = sha
        return sha

    try:
        git(repo, 'read-tree', base, env=env)
        for group in sorted({theme(n) for n in changed}):
            updates = {n: fork_entries.get(n, ('0', '0' * 40)) for n in changed if theme(n) == group}
            update_index(repo, updates, env)
            for n in updates:
                if n in fork_entries:
                    current[n] = fork_entries[n]
                else:
                    current.pop(n, None)
            commit(group, 'Preserves committed fork changes; no upstream upgrade.')
        fork_tree = git(repo, 'write-tree', env=env).decode().strip()
        if fork_tree != assessment['currentFork']['tree']:
            raise ValueError('Reconstructed fork tree differs')
        fork_commit = parent
        replay = []
        managed = sorted(set().union(*(paths(ROOT / item['path']) for item in overlays)))
        with tempfile.TemporaryDirectory(prefix='wine-overlays-', dir=work) as tmp:
            sparse = Path(tmp)
            for name in managed:
                if name in fork_entries:
                    data = git(repo, 'show', fork + ':' + name)
                    dest = sparse / name
                    dest.parent.mkdir(parents=True, exist_ok=True)
                    dest.write_bytes(data)
                    dest.chmod(0o755 if fork_entries[name][0] == '100755' else 0o644)
            for item in overlays:
                patch = ROOT / item['path']
                if hashlib.sha256(patch.read_bytes()).hexdigest() != item['sha256']:
                    raise ValueError('Patch changed during reconstruction')
                with patch.open('rb') as f:
                    included = subprocess.run(['patch', '-d', tmp, '-p1', '--batch', '--force', '--dry-run', '-R'],
                                              stdin=f, capture_output=True).returncode == 0
                if not included:
                    with patch.open('rb') as f:
                        result = subprocess.run(['patch', '-d', tmp, '-p1', '--batch', '--force'], stdin=f, capture_output=True)
                    if result.returncode:
                        raise ValueError(item['path'] + ': ' + result.stdout.decode(errors='replace')[-1200:])
                names = set_files(repo, sparse, paths(patch), current, env)
                sha = commit(Path(item['path']).stem, 'Patch SHA-256: ' + item['sha256'])
                replay.append({**item, 'state': 'already-in-fork' if included else 'replayed',
                               'commit': sha, 'changedFiles': names})
            wine_runtime_diagnostics.apply(sparse)
            set_files(repo, sparse, managed, current, env)
            commit('production-diagnostics-policy', 'scripts/wine_runtime_diagnostics.py')
            for name in managed:
                if (sparse / name).read_bytes() != (source / name).read_bytes():
                    raise ValueError('Effective overlay source differs: ' + name)

        # Include all effective tracked changes and source inputs supplied by the
        # build recipe. Patch backup files are recorded, not treated as source.
        tracked = set(tree_entries(source, 'HEAD'))
        untracked = set(os.fsdecode(n) for n in git(source, 'ls-files', '--others', '--exclude-standard', '-z').split(b'\0') if n)
        auxiliary = sorted(n for n in untracked if n.endswith(('.orig', '.rej')))
        source_names = tracked | (untracked - set(auxiliary))
        expected = {}
        set_files(repo, source, source_names, expected, env)
        # set_files wrote all expected entries; remove stale baseline entries too.
        removed = {n: ('0', '0' * 40) for n in set(current) - set(expected)}
        update_index(repo, removed, env)
        extra = sorted(n for n in set(current) | set(expected) if current.get(n) != expected.get(n))
        commit('build-generated-and-shared-platform-inputs',
               'Preserves effective build inputs; generator equivalence remains a separate gate.')
        effective_tree = git(repo, 'write-tree', env=env).decode().strip()
        # Independently rebuild a fresh full index from actual source bytes.
        git(repo, 'read-tree', '--empty', env=env)
        update_index(repo, expected, env)
        if git(repo, 'write-tree', env=env).decode().strip() != effective_tree:
            raise ValueError('Independent effective source tree differs')
        branch = 'refs/heads/ohos/current-baseline'
        git(repo, 'update-ref', branch, parent)
        if git(repo, 'merge-base', '--is-ancestor', base, parent) != b'':
            raise ValueError('Real Valve parent is not in the maintenance lineage')
        generated = {n: hashlib.sha256((source / n).read_bytes()).hexdigest()
                     for n in ('configure', 'include/config.h.in', 'dlls/winevulkan/vulkan_thunks.c') if (source / n).is_file()}
        vulkan_generation = verify_vulkan_generation(source, work)
        result = {'schemaVersion': 1, 'scope': 'current effective source reconstruction; no upstream upgrade or device test',
                  'baseline': assessment['originalValveBase'], 'sourceFork': assessment['currentFork'],
                  'forkTreeEqual': True, 'effectiveTreeEqual': True, 'valveParentVerified': True,
                  'maintenanceCommit': parent, 'maintenanceTree': effective_tree,
                  'reconstructedForkCommit': fork_commit, 'committedDifferencePaths': len(changed),
                  'commits': commits, 'overlays': replay, 'effectiveFiles': len(expected),
                  'reconciledBuildInputs': extra, 'excludedPatchBackups': auxiliary, 'generatedInputs': generated,
                  'vulkanGeneration': vulkan_generation,
                  'pending': ['migrate FEX and Wine to locked candidate snapshots',
                              'build and runtime regression of migrated candidates'],
                  'localRepository': str(repo), 'localSource': str(source), 'branch': branch}
        report_path.parent.mkdir(parents=True, exist_ok=True)
        report_path.write_text(json.dumps(result, indent=2) + '\n')
        print(json.dumps({k: result[k] for k in ('maintenanceCommit', 'maintenanceTree', 'forkTreeEqual',
                                                'effectiveTreeEqual', 'valveParentVerified', 'effectiveFiles')}))
    finally:
        index.unlink(missing_ok=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source', required=True, type=Path)
    p.add_argument('--work-dir', required=True, type=Path)
    p.add_argument('--report', required=True, type=Path)
    a = p.parse_args()
    reconstruct(a.source, a.work_dir, a.report)


if __name__ == '__main__':
    main()
