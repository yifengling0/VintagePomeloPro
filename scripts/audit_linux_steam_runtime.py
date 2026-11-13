#!/usr/bin/env python3
"""Inventory a verified external GNU runtime without executing any guest ELF.

DT_NEEDED availability is an inventory check, not a simulation of loader search
order or an assertion that optional dlopen features work on the device.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--readelf', default='readelf')
    parser.add_argument('--package-lock', action='append', type=Path, default=[])
    args = parser.parse_args()
    runtime = args.runtime.resolve()
    manifest_path = runtime / 'manifest.json'
    manifest = json.loads(manifest_path.read_text())
    records = {}
    for name, expected in sorted(manifest['files'].items()):
        file = (runtime / name).resolve()
        if not file.is_relative_to(runtime):
            raise ValueError('Outside runtime: ' + name)
        data = file.read_bytes()
        digest = hashlib.sha256(data).hexdigest()
        if len(data) != expected['bytes'] or digest != expected['sha256']:
            raise ValueError('Manifest mismatch: ' + name)
        record = dict(bytes=len(data), sha256=digest)
        if data[:4] == b'\x7fELF':
            dynamic = subprocess.run([args.readelf, '-dW', str(file)], check=True,
                                     text=True, capture_output=True, timeout=10).stdout
            record['needed'] = re.findall(r'\(NEEDED\).*?\[(.*?)\]', dynamic)
            record['soname'] = re.findall(r'\(SONAME\).*?\[(.*?)\]', dynamic)
            record['searchPaths'] = re.findall(r'\((?:RUNPATH|RPATH)\).*?\[(.*?)\]', dynamic)
        records[name] = record
    available = {}
    for name, record in records.items():
        for soname in [Path(name).name, *record.get('soname', [])]:
            available.setdefault(soname, []).append(name)
    missing = {}
    for name, record in records.items():
        absent = sorted(set(record.get('needed', [])) - available.keys())
        if absent:
            missing[name] = absent
    packages = {}
    def collect(value):
        if isinstance(value, dict):
            if all(k in value for k in ['Package', 'Version', 'Architecture', 'SHA256']):
                key = value['Package'] + ':' + value['Architecture'] + '@' + value['Version']
                packages[key] = {k: value[k] for k in
                                 ['Package', 'Version', 'Architecture', 'Filename', 'SHA256'] if k in value}
            for nested in value.values():
                collect(nested)
        elif isinstance(value, list):
            for nested in value:
                collect(nested)
    collect(manifest)
    for lock in args.package_lock:
        collect(json.loads(lock.read_text()))
    result = dict(schemaVersion=1, scope='external-runtime-inventory-not-loader-emulation',
                  manifestSha256=hashlib.sha256(manifest_path.read_bytes()).hexdigest(),
                  fileCount=len(records), elfCount=sum('needed' in r for r in records.values()),
                  totalBytes=sum(r['bytes'] for r in records.values()),
                  glibcVersion=manifest['gnuRuntime']['version'],
                  xwaylandVersion=manifest['xwaylandProbe']['version'],
                  mesaCommit=manifest['mesaBuild']['mesaCommit'],
                  originalSteamSha256=manifest['steamClientProbe']['steamSha256'],
                  packages=dict(sorted(packages.items())), files=records,
                  neededNotPresentInInventory=missing,
                  limitations=['Includes optional Valve tools and diagnostics, not only active processes.',
                               'Does not infer dlopen, kernel support or hardware acceleration from file presence.',
                               'GNU/OHOS shared libraries remain separate ABI domains.'])
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2, ensure_ascii=False) + '\n')
    print(json.dumps({k: result[k] for k in ['fileCount', 'elfCount', 'totalBytes',
                                            'neededNotPresentInInventory']}))


if __name__ == '__main__':
    main()
