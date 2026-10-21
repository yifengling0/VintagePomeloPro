#!/usr/bin/env python3
"""Stage self-built Steam bridge + matching patched Wine modules into wine-data.zip.

Valve payloads and Windows Steam are never imported. Call before assembleHap.
The expected base digest prevents applying experimental kernelbase binaries to
an unrelated Wine runtime. Normal Wine builds already apply patch 0065.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import zipfile

ROOT = Path(__file__).resolve().parents[1]

def sha(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(4 * 1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()

def validate_pe(path, machines, kernelbase=False):
    data = path.read_bytes()
    if not 1024 <= len(data) <= 16777216 or data[:2] != b'MZ':
        raise ValueError('Invalid bridge PE: ' + str(path))
    offset = struct.unpack_from('<I', data, 60)[0]
    if offset > len(data) - 24 or data[offset:offset+4] != b'PE\0\0' or struct.unpack_from('<H', data, offset+4)[0] not in machines:
        raise ValueError('Wrong bridge architecture: ' + str(path))
    if kernelbase and (data[64:80] != b'Wine builtin DLL' or 'HMOSDECK_STEAM_BRIDGE'.encode('utf-16le') not in data):
        raise ValueError('Wine kernelbase lacks the scoped Linux Steam session patch')

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rawfile-dir', required=True, type=Path)
    parser.add_argument('--bridge-output', required=True, type=Path)
    parser.add_argument('--kernelbase-i386', required=True, type=Path)
    parser.add_argument('--kernelbase-aarch64', required=True, type=Path)
    parser.add_argument('--expected-payload-sha256', required=True)
    args = parser.parse_args()
    archive = args.rawfile_dir / 'wine-data.zip'
    runtime_manifest = args.rawfile_dir / 'wine-runtime-manifest.json'
    version = args.rawfile_dir / 'wine_runtime.version'
    manifest = json.loads(runtime_manifest.read_text())
    current = sha(archive)
    if manifest['payloadSha256'] not in {current, args.expected_payload_sha256}:
        parser.error('Wine base payload digest differs; do not mix Wine builds')
    built = json.loads((args.bridge_output / 'manifest.json').read_text())
    if built['gnuTarget'] != 'aarch64-none-linux-gnu':
        parser.error('Expected native GNU ARM64 bridge counterpart')
    modules = {
        'bin/i386-windows/kernelbase.dll': args.kernelbase_i386,
        'bin/i386-windows/lsteamclient.dll': args.bridge_output/'i686/lsteamclient.dll',
        'bin/aarch64-windows/kernelbase.dll': args.kernelbase_aarch64,
        'bin/aarch64-windows/lsteamclient.dll': args.bridge_output/'x86_64/lsteamclient.dll',
    }
    for name, path in modules.items():
        kernel = name.endswith('/kernelbase.dll')
        machines = {0x14c} if '/i386-' in name else ({0xaa64, 0xa64e} if kernel else {0x8664})
        validate_pe(path, machines, kernel)
        if not kernel:
            key = ('i686/' if '/i386-' in name else 'x86_64/') + 'lsteamclient.dll'
            if sha(path) != built['files'][key]['sha256']:
                parser.error('PE bridge build manifest mismatch')
    marker = {'schemaVersion': 1, 'source': 'self_built_linux_steam_bridge',
              'files': [{'path': name, 'bytes': path.stat().st_size, 'sha256': sha(path)} for name,path in modules.items()],
              'gnuSha256': built['files']['steamohos_gnu_steam_bridge']['sha256'],
              'registryPatchSha256': sha(ROOT/'patches/wine/0065-kernelbase-linux-steam-session-registry.patch')}
    encoded = (json.dumps(marker, indent=2) + '\n').encode()
    marker_name = 'linux-steam-bridge.json'
    temporary = archive.with_suffix('.steam-pending.zip')
    with zipfile.ZipFile(archive) as staged:
        installed = marker_name in staged.namelist() and staged.read(marker_name) == encoded and all(
            hashlib.sha256(staged.read(record['path'])).hexdigest() == record['sha256'] for record in marker['files'])
    if current != args.expected_payload_sha256 and not installed:
        parser.error('Wine base payload digest differs; do not mix Wine builds')
    try:
        digest = current
        if not installed:
            with zipfile.ZipFile(archive) as source, zipfile.ZipFile(temporary,'w') as target:
                for item in source.infolist():
                    if item.filename in modules or item.filename == marker_name:
                        continue
                    with source.open(item) as reader, target.open(item,'w') as writer:
                        shutil.copyfileobj(reader, writer, 4 * 1024 * 1024)
                for name,path in modules.items():
                    target.write(path,name,compress_type=zipfile.ZIP_DEFLATED,compresslevel=6)
                target.writestr(marker_name,encoded,compress_type=zipfile.ZIP_DEFLATED)
            with zipfile.ZipFile(temporary) as staged:
                for record in marker['files']:
                    if hashlib.sha256(staged.read(record['path'])).hexdigest() != record['sha256']:
                        raise ValueError('Incomplete staged bridge module')
            digest = sha(temporary)
        text = version.read_text().strip()
        if not text.startswith('content='):
            raise ValueError('Unrecognized runtime version marker')
        text = re.sub(r'^content=[0-9a-f]+', 'content=' + digest[:16], text)
        text = re.sub(r';linux-steam-bridge=[0-9a-f]+', '', text)
        text += ';linux-steam-bridge=' + hashlib.sha256(encoded).hexdigest()[:16] + '\n'
        manifest['payloadSha256'] = digest
        manifest['linuxSteamBridge'] = marker
        if temporary.exists(): temporary.replace(archive)
        for path, value in [(runtime_manifest, json.dumps(manifest,indent=2)+'\n'), (version, text)]:
            pending = path.with_suffix(path.suffix + '.steam-pending')
            pending.write_text(value)
            pending.replace(path)
        print(json.dumps({'payloadSha256': digest, 'modules': len(modules), 'windowsSteamRequired': False}))
    finally:
        temporary.unlink(missing_ok=True)

if __name__ == '__main__': main()
