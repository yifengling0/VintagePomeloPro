#!/usr/bin/env python3
"""Prove an overlapped Wine patch against a clean, fully replayed overlay stack.

Only managed files are copied. Caller source, index and cache are never reset.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile
from wine_build_identity import ordered_overlays
import wine_runtime_diagnostics

def paths(patch):
    return {line[6:].split('\t')[0] for line in patch.read_text().splitlines()
            if line.startswith('+++ b/')}

def verify(source, repo, target=None):
    overlays = [repo / item['path'] for item in ordered_overlays(repo)]
    if target is not None and target.resolve() not in [p.resolve() for p in overlays]:
        raise ValueError('patch is not in the managed stack')
    names = sorted(set().union(*(paths(p) for p in overlays)))
    with tempfile.TemporaryDirectory(prefix='overlay-proof-', dir=repo / 'build') as tmp:
        root = Path(tmp)
        for name in names:
            result = subprocess.run(['git', '-C', str(source), 'show', 'HEAD:' + name],
                                    capture_output=True)
            if result.returncode == 0:
                dest = root / name
                dest.parent.mkdir(parents=True, exist_ok=True)
                dest.write_bytes(result.stdout)
        for patch in overlays:
            with patch.open('rb') as data:
                already = subprocess.run(['patch', '-d', str(root), '-p1', '--batch', '--force', '--dry-run', '-R'],
                    stdin=data, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0
            if already: continue
            with patch.open('rb') as data:
                result = subprocess.run(['patch', '-d', str(root), '-p1', '--batch', '--force'],
                               stdin=data, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
                if result.returncode:
                    raise ValueError(str(patch) + ': ' + result.stdout.decode(errors='replace')[-3000:])
        wine_runtime_diagnostics.apply(root)
        for name in (paths(target) if target else names):
            if (root / name).read_bytes() != (source / name).read_bytes():
                raise ValueError('effective overlay differs: ' + name)

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', required=True, type=Path)
    parser.add_argument('--repo', required=True, type=Path)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--patch', type=Path)
    mode.add_argument('--all', action='store_true')
    args = parser.parse_args()
    try:
        verify(args.source.resolve(), args.repo.resolve(), args.patch.resolve() if args.patch else None)
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        parser.exit(1, str(error) + '\n')
