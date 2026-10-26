#!/usr/bin/env python3
"""Build the OHOS Wine half of the Linux Steam bridge during normal assembly.

The matching GNU service is a pinned component. Reuse the existing locked SDK
generator and PE flags; never substitute GNU Proton's different lsteamclient.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
from zipfile import ZipFile

from fex_simd_runtime import pe_kind
from generate_linux_steam_bridge import generate
from prepare_linux_runtime_components import digest, expected_members

ROOT = Path(__file__).resolve().parents[1]
MARKER = 'linux-steam-bridge.json'
MODULES = {
    'bin/i386-windows/kernelbase.dll': ('x86',),
    'bin/i386-windows/lsteamclient.dll': ('x86',),
    'bin/aarch64-windows/kernelbase.dll': ('arm64', 'arm64ec', 'arm64x'),
    'bin/aarch64-windows/lsteamclient.dll': ('x64',),
}
BRIDGES = {'i686': 'bin/i386-windows/lsteamclient.dll',
           'x86_64': 'bin/aarch64-windows/lsteamclient.dll'}
PATCH = 'patches/wine/0065-kernelbase-linux-steam-session-registry.patch'


def sha(data):
    return hashlib.sha256(data).hexdigest()


def file_sha(path):
    with path.open('rb') as stream:
        return digest(stream)


def gnu_bridge(rawfile, verify_archive=False):
    lock = json.loads((ROOT / 'docs/linux-steam/build-components.lock.json').read_text())
    row = next(r for r in lock['components'] if r['path'].startswith('linux-steam/'))
    members = expected_members(ROOT, row)
    member = 'release/runtime/steamohos_gnu_steam_bridge'
    expected = members[member]
    if verify_archive:
        path = rawfile / row['path']
        if path.stat().st_size != row['bytes'] or file_sha(path) != row['sha256']:
            raise ValueError('GNU bridge component differs from build lock')
        with ZipFile(path) as archive:
            data = archive.read(member)
        if len(data) != expected['bytes'] or sha(data) != expected['sha256']:
            raise ValueError('GNU bridge service differs from component manifest')
    return expected['sha256']


def validate_module(name, data):
    if not 1024 <= len(data) <= 16 * 1024 * 1024 or pe_kind(data) not in MODULES[name]:
        raise ValueError('Invalid Linux Steam bridge module: ' + name)
    if name.endswith('/kernelbase.dll') and (
            data[64:80] != b'Wine builtin DLL' or
            'HMOSDECK_STEAM_BRIDGE'.encode('utf-16le') not in data):
        raise ValueError('Kernelbase lacks the Linux Steam session patch: ' + name)


def validate_payload(read, expected_gnu=None, candidate=None):
    marker = json.loads(read(MARKER))
    records = marker.get('files', [])
    if (marker.get('schemaVersion') != 1 or marker.get('source') != 'self_built_linux_steam_bridge' or
            len(records) != len(MODULES) or {r['path'] for r in records} != set(MODULES) or
            marker.get('registryPatchSha256') != sha((ROOT / PATCH).read_bytes()) or
            marker.get('gnuSha256') != (expected_gnu or gnu_bridge(None))):
        raise ValueError('Linux Steam bridge inventory or GNU service pairing is invalid')
    for item in records:
        data = read(item['path'])
        validate_module(item['path'], data)
        if len(data) != item['bytes'] or sha(data) != item['sha256']:
            raise ValueError('Unpaired Linux Steam bridge module: ' + item['path'])
        if candidate is not None:
            from source_build_contract import validate_member
            validate_member(candidate, 'payload', item['path'], data)


def build(mingw, reference, output, offline=False):
    source = ROOT / 'entry/src/main/cpp/linux_steam/steam_bridge'
    inputs = [p for p in sorted(source.iterdir()) if p.is_file()]
    inputs += [Path(__file__), ROOT / 'scripts/generate_linux_steam_bridge.py',
               ROOT / 'scripts/steam_bridge_codecs.py', ROOT / 'scripts/steam_bridge_contracts.py',
               ROOT / 'docs/linux-steam/steam_bridge_sdk.lock.json']
    identity = {'sources': {str(p.relative_to(ROOT)): file_sha(p) for p in inputs},
                'compilers': {arch: file_sha(mingw / (arch + '-w64-mingw32-clang++')) for arch in BRIDGES}}
    receipt = output / 'pe-build.json'
    if receipt.exists():
        cached = json.loads(receipt.read_text())
        if cached.get('inputs') == identity and all(
                (output / arch / 'lsteamclient.dll').is_file() and
                file_sha(output / arch / 'lsteamclient.dll') == cached.get('files', {}).get(arch)
                for arch in BRIDGES):
            for arch, name in BRIDGES.items():
                validate_module(name, (output / arch / 'lsteamclient.dll').read_bytes())
            print('Linux Steam PE bridge: verified incremental outputs reused')
            return
    fetch = [sys.executable, str(ROOT / 'scripts/fetch_linux_steam_bridge_sdk.py'),
             '--reference', str(reference), *(['--offline'] if offline else [])]
    for attempt in range(1 if offline else 3):
        result = subprocess.run(fetch)
        if result.returncode == 0: break
    result.check_returncode()
    generated = output / 'generated'
    generate(reference, mingw / 'x86_64-w64-mingw32-clang++', generated)
    flags = ['-std=c++17', '-O2', '-Wall', '-Wextra', '-Wno-unused-parameter',
             '-Wno-misleading-indentation', '-isystem', str(reference / 'lsteamclient/steamworks_sdk_150'),
             '-I' + str(generated), '-fms-extensions', '-shared', '-static']
    hashes = {}
    for arch, name in BRIDGES.items():
        target = output / arch / 'lsteamclient.dll'; target.parent.mkdir(parents=True, exist_ok=True)
        pending = target.with_suffix('.pending.dll')
        subprocess.run([str(mingw / (arch + '-w64-mingw32-clang++')), *flags,
                        str(source / 'pe.cpp'), '-lws2_32', '-Wl,--kill-at', '-o', str(pending)], check=True)
        data = pending.read_bytes(); validate_module(name, data)
        pending.replace(target); hashes[arch] = sha(data)
    receipt.write_text(json.dumps({'inputs': identity, 'files': hashes}, indent=2) + '\n')


def stage(directory, output, rawfile):
    gnu_sha = gnu_bridge(rawfile, verify_archive=True)
    # Validate both new PE bridges and existing source-built kernelbase before
    # updating either bridge. The INF identity is stamped by assemble afterwards.
    data = {name: ((output / arch / 'lsteamclient.dll').read_bytes())
            for arch, name in BRIDGES.items()}
    for name in MODULES:
        if name not in data: data[name] = (directory / name).read_bytes()
        validate_module(name, data[name])
    for name in BRIDGES.values():
        target = directory / name; target.parent.mkdir(parents=True, exist_ok=True)
        pending = target.with_suffix('.pending.dll'); pending.write_bytes(data[name]); pending.replace(target)
    marker = {'schemaVersion': 1, 'source': 'self_built_linux_steam_bridge',
              'files': [dict(path=name, bytes=len(data[name]), sha256=sha(data[name])) for name in MODULES],
              'gnuSha256': gnu_sha, 'registryPatchSha256': sha((ROOT / PATCH).read_bytes())}
    (directory / MARKER).write_text(json.dumps(marker, indent=2) + '\n')
    validate_payload(lambda name: (directory / name).read_bytes(), gnu_sha)
    print('Linux Steam OHOS Wine bridge: both PE modules paired with kernelbase and GNU service')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mingw', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--reference', type=Path)
    parser.add_argument('--directory', type=Path, required=True)
    parser.add_argument('--rawfile', type=Path, required=True)
    parser.add_argument('--offline', action='store_true')
    args = parser.parse_args()
    build(args.mingw, args.reference or args.output / 'reference', args.output, args.offline)
    stage(args.directory, args.output, args.rawfile)
