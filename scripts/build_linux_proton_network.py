#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Rebuild GE's Unix NSI module with checked interface-query error paths."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess
import urllib.request
from proton_network_contract import validate_network_build

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('cc', 'wine-source', 'wine-build', 'widl', 'package'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--source-cache', type=Path)
    args = parser.parse_args()
    lock_path = ROOT / 'docs/linux-steam/gnu-proton-network.lock.json'
    ge_path = ROOT / 'docs/linux-steam/gnu-proton-probe.lock.json'
    lock = json.loads(lock_path.read_text())
    ge = json.loads(ge_path.read_text())
    if lock['protonTag'] != ge['distribution']['tag']:
        raise ValueError('GE network version mismatch')
    if subprocess.check_output([args.cc, '-dumpmachine'], text=True, timeout=10).strip() != 'aarch64-none-linux-gnu':
        raise ValueError('AArch64 GNU compiler required')
    task = ROOT / 'build/gnu-proton-network'
    source = task / 'source/dlls/nsiproxy.sys'
    include = task / 'include'
    source.mkdir(parents=True, exist_ok=True)
    include.mkdir(exist_ok=True)
    headers = args.wine_source.resolve(strict=True) / 'include'
    nt = args.package / 'files/lib/wine/aarch64-unix/ntdll.so'
    original = nt.with_name('nsiproxy.so')
    selected = {row['path']: row for row in ge['selected']}
    for path in (nt, original):
        row = selected[str(path.relative_to(args.package))]
        if path.stat().st_size != row['bytes'] or sha(path) != row['sha256']:
            raise ValueError('GE network binary identity mismatch')
    receipt_path = task / 'build.json'
    arguments = {name: str(getattr(args, name).resolve()) for name in
                 ('cc', 'wine_source', 'wine_build', 'widl', 'package')}
    if receipt_path.is_file():
        try:
            _, previous = validate_network_build(receipt_path)
            if previous.get('arguments') != arguments:
                raise ValueError('GNU NSI build arguments changed')
        except (ValueError, OSError, KeyError):
            pass
        else:
            print('Reused verified GE network module')
            return
    # Validate every original before patching. Do not modify a reference tree.
    for name, row in lock['sources'].items():
        cache = task / 'cache' / name
        if args.source_cache:
            existing = args.source_cache / name
            if not existing.is_file():
                existing = args.source_cache / ('ge-' + row['path'].replace('/', '-'))
            if existing.is_file():
                cache = existing
        if not cache.exists():
            cache.parent.mkdir(exist_ok=True)
            url = f"https://raw.githubusercontent.com/ValveSoftware/wine/{lock['wineRevision']}/{row['path']}"
            data = urllib.request.urlopen(url, timeout=30).read(row['bytes'] + 1)
            if len(data) != row['bytes'] or hashlib.sha256(data).hexdigest() != row['sha256']:
                raise ValueError('Unverified GE source download: ' + name)
            cache.write_bytes(data)
        if cache.stat().st_size != row['bytes'] or sha(cache) != row['sha256']:
            raise ValueError('GE network source identity mismatch: ' + name)
        (source / name).write_bytes(cache.read_bytes())
    for patch in lock['patches']:
        path = ROOT / patch['path']
        if sha(path) != patch['sha256']:
            raise ValueError('GE network patch identity mismatch')
        subprocess.run(['patch', '--batch', '--fuzz=0', '-p1', '-i', path.resolve()],
                       cwd=task / 'source', check=True, timeout=20)
    flags = [str(args.cc), '-O2', '-g0', '-fPIC', '-D_GNU_SOURCE', '-D__WINESRC__',
             '-DWINE_UNIX_LIB', '-D_NTSYSTEM_', '-I' + str(include),
             '-I' + str(args.wine_build / 'include'), '-I' + str(headers), '-I' + str(source)]
    sources = [source / (name + '.c') for name in ('icmp_echo', 'ip', 'ndis', 'nsi', 'tcp', 'udp')]
    output = task / 'nsiproxy.so'
    command = flags + ['-shared', *map(str, sources), '-L' + str(nt.parent), '-l:ntdll.so', '-pthread',
                       '-Wl,-z,relro,-z,now', '-Wl,--no-undefined', '-Wl,-soname,nsiproxy.so', '-o', str(output)]
    with (task / 'build.log').open('w') as log:
        for _ in range(80):
            result = subprocess.run(command, capture_output=True, text=True, timeout=90)
            log.write(result.stdout + result.stderr)
            if not result.returncode:
                break
            missing = re.search(r'fatal error: ([\w/.-]+)\.h: No such file', result.stderr)
            if not missing:
                raise RuntimeError(result.stderr[-5000:])
            name = missing[1]
            idl = headers / (name + '.idl')
            if not idl.is_file():
                raise ValueError('Missing Wine header: ' + name)
            target = include / (name + '.h')
            target.parent.mkdir(parents=True, exist_ok=True)
            subprocess.run([args.widl, '-m64', '-I' + str(headers), '-h', '-o', target, idl],
                           check=True, stdout=log, stderr=log, timeout=30)
        else:
            raise ValueError('Wine network header closure limit')
    def slots(path):
        symbols = subprocess.check_output(['readelf', '-Ws', path], text=True, timeout=10)
        return {name: int(size) // 8 for size, name in re.findall(
            r'\S+\s+(\d+)\s+OBJECT\s+GLOBAL\s+DEFAULT\s+\d+\s+(__wine_unix_call\w+)', symbols)}
    if slots(output) != slots(original) or slots(output).get('__wine_unix_call_funcs') != lock['unixCallSlots']:
        raise ValueError('GE network Unix-call ABI mismatch')
    dependencies = {}
    for file in sources:
        listing = subprocess.check_output(flags + ['-MM', str(file)], text=True, timeout=15)
        for p in shlex.split(listing.replace('\\\n', ' ').split(':', 1)[1]):
            path = Path(p).resolve(strict=True)
            dependencies[str(path)] = sha(path)
    inputs = {str(p.resolve()): sha(p) for p in (Path(__file__), ROOT / 'scripts/proton_network_contract.py',
                                                lock_path, args.cc, args.widl, nt, original,
                                                args.wine_build / 'include/config.h')}
    for row in lock['patches']:
        inputs[str((ROOT / row['path']).resolve())] = row['sha256']
    receipt = dict(inputs=inputs, dependencies=dependencies, arguments=arguments, bytes=output.stat().st_size,
                   sha256=sha(output), unixCallTables=slots(output), wineRevision=lock['wineRevision'])
    (task / 'build.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print('Built GE network module', output.stat().st_size, sha(output))


if __name__ == '__main__':
    main()
