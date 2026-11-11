"""Verify the source-bound, rebuildable runtime32 helper shipped in each HAP.

Rebuild with Ubuntu MinGW GCC 13-win32 via --rebuild. Normal packaging consumes
the tracked 235 KiB helper and refuses a stale source or payload identity.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]
ASSET = 'entry/src/main/resources/rawfile/windows-steam'
SOURCE = 'scripts/steam_legacy_zstd/vpsteam_runtime32.c'


def digest(data):
    return hashlib.sha256(data).hexdigest()


def verify(root=ROOT, hap=None):
    directory = root / ASSET
    manifest = json.loads((directory / 'manifest.json').read_text())
    if manifest['sourceSha256'] != digest((root / SOURCE).read_bytes()):
        raise ValueError('Windows Steam runtime source changed; rebuild the shipped helper')
    data = (directory / 'vpsteamtrust32.dll').read_bytes()
    if len(data) != manifest['size'] or digest(data) != manifest['helperSha256']:
        raise ValueError('Windows Steam helper payload identity mismatch')
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    if data[:2] != b'MZ' or data[pe:pe+4] != b'PE\0\0' or struct.unpack_from('<H', data, pe+4)[0] != 0x14c:
        raise ValueError('Windows Steam helper must be i386 PE')
    if struct.unpack_from('<I', data, pe+8)[0] != 0:
        raise ValueError('Windows Steam helper timestamp must be deterministic')
    if hap:
        with zipfile.ZipFile(hap) as archive:
            for name in ('manifest.json', 'vpsteamtrust32.dll'):
                if archive.read('resources/rawfile/windows-steam/' + name) != (directory / name).read_bytes():
                    raise ValueError('HAP has a stale Windows Steam resource: ' + name)
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rebuild', action='store_true')
    parser.add_argument('--verify-only', action='store_true')
    parser.add_argument('--hap', type=Path)
    args = parser.parse_args()
    if args.rebuild:
        directory = ROOT / ASSET
        manifest = json.loads((directory / 'manifest.json').read_text())
        temporary = ROOT / 'workspace_temp/windows-steam-runtime/vpsteamtrust32.dll'
        temporary.parent.mkdir(parents=True, exist_ok=True)
        command = ['i686-w64-mingw32-gcc', '-O2', '-Wall', '-Wextra', '-Werror',
                   '-static-libgcc', '-Wl,--no-insert-timestamp', '-shared', SOURCE,
                   '-lbcrypt', '-Wl,--image-base,0x6fa40000', '-Wl,--exclude-all-symbols']
        compiler = subprocess.check_output([command[0], '-dumpfullversion'], text=True).strip()
        if compiler != manifest['compilerVersion']:
            raise ValueError('Use the recorded MinGW compiler: ' + manifest['compilerVersion'])
        try:
            subprocess.run(command + ['-o', str(temporary)], cwd=ROOT, check=True)
            manifest.update(sourceSha256=digest((ROOT / SOURCE).read_bytes()),
                            helperSha256=digest(temporary.read_bytes()), size=temporary.stat().st_size)
            temporary.replace(directory / 'vpsteamtrust32.dll')
            (directory / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
        finally:
            temporary.unlink(missing_ok=True)
    manifest = verify(hap=args.hap)
    print('Windows Steam helper verified: ' + manifest['helperSha256'])


if __name__ == '__main__':
    main()
