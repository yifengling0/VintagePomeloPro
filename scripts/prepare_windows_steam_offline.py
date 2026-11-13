#!/usr/bin/env python3
"""Prepare and validate the immutable Windows Steam import ZIP for VPP 2.x.

The archive is reused unchanged. The app upgrades its exact stopped helper
from bundled resources; account, game and cache directories are forbidden.
"""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import tempfile
import urllib.request
from zipfile import ZipFile

from prepare_windows_steam_runtime import verify as verify_runtime

ROOT = Path(__file__).resolve().parents[1]
LOCK = 'docs/windows-steam/offline-package.lock.json'
REPORT = 'release-offline-packages.json'
PRIVATE = {'config', 'userdata', 'appcache', 'logs', 'dumps', 'steamapps',
           'htmlcache', 'depotcache', 'shadercache', 'crashpad', 'loginusers.vdf'}
REQUIRED = ('steam.exe', 'bin/SDL2.dll', 'steam/games/PlatformMenu.vdf',
            'steam/games/SteamMovie.ico', 'steam.cfg')


def stream_hash(stream):
    digest = hashlib.sha256()
    for chunk in iter(lambda: stream.read(1024 * 1024), b''):
        digest.update(chunk)
    return digest.hexdigest()


def file_hash(path):
    with path.open('rb') as stream:
        return stream_hash(stream)


def read_lock(root):
    lock = json.loads((root / LOCK).read_text(encoding='utf-8'))
    name = lock['fileName']
    if (not name or PurePosixPath(name).name != name or '\\' in name or ':' in name or
            any(ord(c) < 32 for c in name) or not name.endswith('.zip')):
        raise ValueError('Invalid Windows Steam archive filename')
    if not re.fullmatch(r'[0-9a-f]{64}', lock['sha256']) or lock['bytes'] <= 0:
        raise ValueError('Invalid Windows Steam archive identity')
    return lock


def verify_archive(root, archive, hap=None, app=None):
    lock = read_lock(root)
    if archive.is_symlink() or not archive.is_file() or archive.stat().st_size != lock['bytes'] or file_hash(archive) != lock['sha256']:
        raise ValueError('Windows Steam archive identity mismatch')
    identity = verify_runtime(root, hap=hap)
    with ZipFile(archive) as package:
        members = package.infolist()
        if len(members) != lock['files'] or sum(item.file_size for item in members) != lock['expandedBytes']:
            raise ValueError('Windows Steam archive inventory mismatch')
        names = set()
        hashes = {}
        for item in members:
            name = item.filename
            path = PurePosixPath(name)
            if (not name.startswith('Steam/') or item.is_dir() or '\\' in name or ':' in name or
                    any(part in ('', '.', '..') for part in name.split('/')) or
                    any(ord(c) < 32 for c in name) or stat.S_ISLNK(item.external_attr >> 16)):
                raise ValueError('Unsafe Windows Steam archive entry: ' + name)
            if name.casefold() in names:
                raise ValueError('Duplicate Windows Steam archive entry: ' + name)
            names.add(name.casefold())
            if any(part.casefold() in PRIVATE or part.casefold().startswith('ssfn') for part in path.parts[1:]):
                raise ValueError('Private Windows Steam archive entry: ' + name)
            with package.open(item) as source:
                hashes[name[6:]] = stream_hash(source)  # also verifies each ZIP CRC
        for name in REQUIRED:
            if name not in hashes:
                raise ValueError('Missing Windows Steam client resource: ' + name)
        for row in identity['files']:
            if hashes.get(row['name']) != row['sha256']:
                raise ValueError('Windows Steam runtime dependency mismatch: ' + row['name'])
        if hashes.get('vpsteamtrust32.dll') not in (identity['previousHelperSha256'], identity['helperSha256']):
            raise ValueError('Windows Steam helper has no managed upgrade path')
        checksums = {}
        for line in package.read('Steam/SHA256SUMS.txt').decode('utf-8-sig').splitlines():
            digest, name = line.split('  ', 1)
            if name in checksums or hashes.get(name) != digest:
                raise ValueError('Windows Steam member checksum mismatch: ' + name)
            checksums[name] = digest
        if set(checksums) != set(hashes) - {'SHA256SUMS.txt'}:
            raise ValueError('Incomplete Windows Steam member checksums')
    checked = [str(hap)] if hap else []
    if app:
        with ZipFile(app) as container:
            haps = [name for name in container.namelist() if name.endswith('.hap')]
            if len(haps) != 1:
                raise ValueError('Expected one entry HAP in the App')
            with tempfile.TemporaryFile() as embedded:
                with container.open(haps[0]) as source:
                    shutil.copyfileobj(source, embedded, length=1024 * 1024)
                embedded.seek(0)
                verify_runtime(root, hap=embedded)
        checked.append(str(app))
    return dict(schemaVersion=2, status='PASS', product='OHOS Wine / Windows Steam',
                packages=[dict(kind='Windows Steam', fileName=archive.name,
                               bytes=lock['bytes'], sha256=lock['sha256'])],
                archiveFiles=len(hashes), helperSha256=identity['helperSha256'],
                checkedPackages=checked,
                applicationArtifacts=[dict(path=str(path), bytes=path.stat().st_size,
                                           sha256=file_hash(path)) for path in (hap, app) if path])


def prepare(root, output, offline=False):
    lock = read_lock(root)
    archive = output / lock['fileName']
    if archive.exists():
        return archive  # verifier rejects corrupt/foreign bytes without replacing them
    if offline:
        raise ValueError('Missing pinned Windows Steam archive in ' + str(output))
    if not lock['url'].startswith('https://github.com/yifengling0/VintagePomeloPro/releases/download/'):
        raise ValueError('Unapproved Windows Steam archive URL')
    output.mkdir(parents=True, exist_ok=True)
    pending = archive.with_suffix('.pending')
    try:
        with urllib.request.urlopen(lock['url'], timeout=45) as source, pending.open('wb') as target:
            copied = 0
            for data in iter(lambda: source.read(1024 * 1024), b''):
                copied += len(data)
                if copied > lock['bytes']:
                    raise ValueError('Windows Steam download exceeds pinned size')
                target.write(data)
        if pending.stat().st_size != lock['bytes'] or file_hash(pending) != lock['sha256']:
            raise ValueError('Downloaded Windows Steam archive identity mismatch')
        pending.replace(archive)
    finally:
        pending.unlink(missing_ok=True)
    return archive


def write_report(output, result):
    output.mkdir(parents=True, exist_ok=True)
    (output / REPORT).write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    (output / 'SHA256SUMS-current.txt').write_text(''.join(
        row['sha256'] + '  ' + row['fileName'] + '\n' for row in result['packages']), encoding='utf-8')
    (output / 'OFFLINE-IMPORT.txt').write_text(
        'Windows Steam 离线导入\n\nSteam 组件管理 → 导入 Windows Steam ZIP，选择：\n' +
        result['packages'][0]['fileName'] + '\n\n无需解压。包不含账号、游戏、缓存和存档。\n'
        '应用在启动前更新配套辅助模块。首次登录和下载游戏仍需联网。\n'
        'VPP 2.0.0 使用 OHOS Wine / FEX；本发布不提供 Linux Steam / GNU Proton 包。\n', encoding='utf-8')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', required=True, type=Path)
    parser.add_argument('--archive', type=Path, help='Verify an existing archive in place without copying it')
    parser.add_argument('--offline', action='store_true')
    parser.add_argument('--verify-only', action='store_true')
    parser.add_argument('--hap', type=Path)
    parser.add_argument('--app', type=Path)
    args = parser.parse_args()
    archive = args.archive or (args.output_dir / read_lock(ROOT)['fileName'] if args.verify_only
                               else prepare(ROOT, args.output_dir, args.offline))
    result = verify_archive(ROOT, archive, args.hap, args.app)
    write_report(args.output_dir, result)
    print(json.dumps(result, ensure_ascii=False))


if __name__ == '__main__':
    main()
