#!/usr/bin/env python3
"""Build/reuse the small HAP-owned GNU client supervisor, without repacking Steam."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ['entry/src/main/cpp/linux_steam/client_session_restart.c',
           'entry/src/main/cpp/linux_steam/gnu_fixture/steamclient_session.c']


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cc', default='aarch64-linux-gnu-gcc')
    p.add_argument('--rebuild', action='store_true')
    p.add_argument('--verify-only', action='store_true')
    p.add_argument('--output', type=Path, default=ROOT/'entry/src/main/resources/rawfile/linux-steam/bootstrap')
    a = p.parse_args()
    sources = {name:sha(ROOT/name) for name in SOURCES}
    binary = a.output/'steamohos_gnu_steam_session_hap'
    manifest = a.output/'manifest.json'
    old = json.loads(manifest.read_text()) if manifest.is_file() else {}
    if not a.rebuild and old.get('sources') == sources and binary.is_file() and old.get('sha256') == sha(binary):
        print('Verified/reused HAP GNU Steam supervisor:', binary.stat().st_size, 'bytes'); return
    if a.verify_only:
        raise ValueError('HAP Steam supervisor source/payload changed; rebuild with a GNU ARM64 compiler')
    target = subprocess.check_output([a.cc, '-dumpmachine'], text=True).strip()
    if not target.startswith('aarch64') or 'linux-gnu' not in target:
        raise ValueError('Expected GNU ARM64 compiler')
    a.output.mkdir(parents=True, exist_ok=True)
    pending = binary.with_suffix('.pending')
    subprocess.run([a.cc, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-fPIE', '-pie',
                    '-Wl,-z,relro,-z,now', str(ROOT/SOURCES[0]), '-o', str(pending)], check=True)
    data = pending.read_bytes()
    if data[:6] != b'\x7fELF\x02\x01' or int.from_bytes(data[18:20], 'little') != 183:
        raise ValueError('Supervisor is not GNU ARM64 ELF')
    pending.replace(binary)
    manifest.write_text(json.dumps(dict(schemaVersion=1, architecture='aarch64-linux-gnu',
        name=binary.name, bytes=len(data), sha256=sha(binary), sources=sources), indent=2)+'\n')
    print('Built HAP GNU Steam supervisor:', len(data), 'bytes')


if __name__ == '__main__': main()
