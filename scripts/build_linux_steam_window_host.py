#!/usr/bin/env python3
"""Build/stage the self-owned Wine HWND adapter for ordinary HAP builds."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', required=True, type=Path)
    parser.add_argument('--rawfile-dir', type=Path, default=ROOT/'entry/src/main/resources/rawfile')
    args = parser.parse_args()
    source = ROOT/'tools/steam-boundary/steam_window_host.c'
    directory = args.rawfile_dir/'hmosdeck'; directory.mkdir(parents=True, exist_ok=True)
    target = directory/'hmosdeck-steam.exe'; temporary = target.with_suffix('.tmp.exe')
    try:
        subprocess.run([str(args.cc), '-O2', '-DNDEBUG', '-Wall', '-Wextra', '-Werror',
                        '-municode', '-mwindows', str(source), '-lgdi32', '-o', str(temporary)], check=True)
        data = temporary.read_bytes()
        pe = struct.unpack_from('<I', data, 60)[0]
        if not 1024 <= len(data) <= 524288 or data[:2] != b'MZ' or data[pe:pe+4] != b'PE\0\0':
            raise ValueError('Invalid Wine HWND adapter')
        temporary.replace(target)
        record = {'schemaVersion':1, 'source':str(source.relative_to(ROOT)),
                  'sourceSha256':hashlib.sha256(source.read_bytes()).hexdigest(),
                  'sha256':hashlib.sha256(data).hexdigest(), 'bytes':len(data),
                  'machine':hex(struct.unpack_from('<H', data, pe+4)[0])}
        (directory/'window-host.json').write_text(json.dumps(record,indent=2)+'\n',encoding='utf-8')
        print('Staged Wine HWND adapter:', record['sha256'])
    finally:
        if temporary.exists(): temporary.unlink()

if __name__ == '__main__': main()
