#!/usr/bin/env python3
"""Prepare and verify the exact Steam/Proton import packages for a release.

Run before HAP/App packaging. Unchanged component sets reuse published bytes;
verify the built HAP/App afterwards so release upload cannot omit a new bundle.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import urllib.request
import zipfile
from build_linux_steam_offline_package import build, compatible_bundle, file_hash

ROOT = Path(__file__).resolve().parents[1]
RAW = Path('entry/src/main/resources/rawfile')
REPORT = 'release-offline-packages.json'

def read(path):
    return json.loads(path.read_text(encoding='utf-8'))

def leaf(name):
    if not isinstance(name, str) or not name or len(name) > 160 or '/' in name or '\\' in name or ':' in name or any(ord(c) < 32 for c in name):
        raise ValueError('Invalid offline package filename')
    return name

def expected_archives(plan):
    return {'archives/' + a['sha256'] + '.zip': (a['bytes'], a['sha256'])
            for a in plan['archives'] if not a['url'].startswith('rawfile://')}

def valid(path, size, checksum):
    return path.is_file() and not path.is_symlink() and path.stat().st_size == size and file_hash(path) == checksum

def proton_release(root):
    """Use the inventory consumed by the local-import UI, not the adapter ZIP."""
    raw = root / RAW / 'linux-proton'
    catalog = read(raw / 'runtime-catalog.json')
    if catalog != read(root / 'docs/linux-steam/proton-runtimes/catalog.json'):
        raise ValueError('Bundled and published Proton runtime catalogs differ')
    release = catalog['releases'][0]
    manifest = read(raw / 'manifest.json')
    inventory = lambda rows: {r['path']: (r['bytes'], r['sha256'], r['mode']) for r in rows}
    expected = [r for r in manifest['files'] if r['path'].startswith('package/')]
    if (not expected or len(inventory(release['files'])) != len(release['files']) or
            inventory(expected) != inventory(release['files'])):
        raise ValueError('Proton runtime catalog does not match the bundled upstream files')
    return release

def verify_proton_archive(path, release):
    """Adapter sidecars in the historical full ZIP are never runtime inputs."""
    with zipfile.ZipFile(path) as z:
        names = z.namelist()
        expected = {row['path']: row for row in release['files']}
        if len(set(names)) != len(names) or {n for n in names if n.startswith('package/')} != set(expected):
            raise ValueError('Proton offline package inventory mismatch')
        for name, row in expected.items():
            if z.getinfo(name).file_size != row['bytes']:
                raise ValueError('Proton offline file size mismatch: ' + name)
            with z.open(name) as stream:
                checksum = hashlib.sha256()
                for block in iter(lambda: stream.read(1048576), b''):
                    checksum.update(block)
                if checksum.hexdigest() != row['sha256']:
                    raise ValueError('Proton offline file checksum mismatch: ' + name)

def fetch(archive, destination):
    url = archive['url']
    if not (url.startswith('https://client-update.steamstatic.com/') or
            re.fullmatch(r'https://github\.com/yifengling0/VintagePomeloPro/releases/download/[A-Za-z0-9._-]+/[A-Za-z0-9._-]+\.zip', url)):
        raise ValueError('Unapproved pinned component URL')
    destination.parent.mkdir(parents=True, exist_ok=True)
    pending = destination.with_suffix(destination.suffix + '.pending')
    try:
        with urllib.request.urlopen(url, timeout=45) as source, pending.open('wb') as target:
            copied = 0
            while True:
                data = source.read(1048576)
                if not data: break
                copied += len(data)
                if copied > archive['bytes']: raise ValueError('Official archive exceeds pinned size')
                target.write(data)
        if not valid(pending, archive['bytes'], archive['sha256']):
            raise ValueError('Pinned archive identity mismatch: ' + archive.get('id', url.rsplit('/', 1)[-1]))
        pending.replace(destination)
    finally:
        pending.unlink(missing_ok=True)

def prepare(root, downloads, output, caches=(), allow_download=True):
    raw = root / RAW
    plan = read(raw / 'linux-steam/deployment.json')
    expected = expected_archives(plan)
    archive_set = hashlib.sha256(json.dumps(expected, sort_keys=True).encode()).hexdigest()
    metadata_path = raw / 'linux-steam/offline-package.json'
    previous = read(metadata_path) if metadata_path.is_file() else {}
    name = (leaf(previous['fileName']) if previous.get('archiveSetId') == archive_set else
            'SteamLinux-ARM64-' + leaf(plan['clientBuild']) + '-' + archive_set[:12] + '-offline.zip')
    steam = output / name
    # A complete existing publication is the best cache. Never unpack/copy the
    # same 370 MiB to recreate it under a release-date or app-version filename.
    if steam.is_file() and compatible_bundle(steam, expected):
        if not valid(steam, previous.get('bytes'), previous.get('sha256')):
            raise ValueError('Existing offline publication differs from signed metadata')
    else:
        for archive in plan['archives']:
            if archive['url'].startswith('rawfile://'): continue
            filename = leaf(archive['url'].rsplit('/', 1)[-1])
            candidates = [folder / filename for folder in [downloads, *caches]]
            if any(valid(p, archive['bytes'], archive['sha256']) for p in candidates): continue
            if not allow_download: raise ValueError('Missing cached official archive: ' + archive['id'])
            print('Fetching pinned official archive:', archive['id'], flush=True)
            fetch(archive, candidates[0])
        build(raw / 'linux-steam/deployment.json', downloads, steam, raw / 'linux-steam', archive_caches=caches)
    runtime = proton_release(root)
    proton = runtime['archive']
    version = re.sub(r'[^A-Za-z0-9._-]+', '-', runtime['name'])
    proton_name = leaf(version + '-ARM64-' + proton['sha256'][:12] + '-offline.zip')
    # Reuse any already published exact Proton archive, irrespective of label.
    matches = [p for p in output.glob('*.zip') if p != steam and valid(p, proton['bytes'], proton['sha256'])]
    target = matches[0] if matches else output / proton_name
    if not valid(target, proton['bytes'], proton['sha256']):
        filename = leaf(proton['url'].rsplit('/', 1)[-1])
        candidates = [raw / 'linux-proton/gnu-proton.zip',
                      *[folder / name for folder in [downloads, *caches]
                        for name in [filename, proton['sha256'] + '.zip']]]
        source = next((p for p in candidates if valid(p, proton['bytes'], proton['sha256'])), None)
        if source is None:
            if not allow_download: raise ValueError('Missing cached Proton runtime archive from the import catalog')
            source = downloads / (proton['sha256'] + '.zip')
            if not valid(source, proton['bytes'], proton['sha256']): fetch(proton, source)
        output.mkdir(parents=True, exist_ok=True)
        pending = target.with_suffix('.pending')
        try:
            shutil.copyfile(source, pending)
            if not valid(pending, proton['bytes'], proton['sha256']): raise ValueError('Proton copy failed validation')
            pending.replace(target)
        finally: pending.unlink(missing_ok=True)
    return verify(root, output, proton_name=target.name)

def verify(root, output, hap=None, app=None, proton_name=None):
    raw = root / RAW
    plan = read(raw / 'linux-steam/deployment.json')
    metadata = read(raw / 'linux-steam/offline-package.json')
    steam = output / leaf(metadata['fileName'])
    expected = expected_archives(plan)
    archive_set = hashlib.sha256(json.dumps(expected, sort_keys=True).encode()).hexdigest()
    if metadata.get('archiveSetId') != archive_set or not valid(steam, metadata['bytes'], metadata['sha256']) or not compatible_bundle(steam, expected):
        raise ValueError('Release Steam offline package is missing, stale or corrupt')
    runtime = proton_release(root)
    proton = runtime['archive']
    if proton_name:
        tool = output / leaf(proton_name)
    else:
        tools = [p for p in output.glob('*.zip') if p != steam and valid(p, proton['bytes'], proton['sha256'])]
        if len(tools) != 1: raise ValueError('Expected one matching release Proton offline package')
        tool = tools[0]
    if not valid(tool, proton['bytes'], proton['sha256']): raise ValueError('Release Proton offline package mismatch')
    verify_proton_archive(tool, runtime)
    checked = []
    def check_package(package):
        with zipfile.ZipFile(package) as z:
            if json.loads(z.read('resources/rawfile/linux-steam/offline-package.json')) != metadata:
                raise ValueError('HAP accepts a different Steam offline package; rebuild HAP after preparing offline packages')
            if json.loads(z.read('resources/rawfile/linux-proton/runtime-catalog.json')) != read(raw / 'linux-proton/runtime-catalog.json'):
                raise ValueError('HAP Proton identity differs from release offline package')
        checked.append(str(package) if isinstance(package, Path) else 'App embedded HAP')
    if hap: check_package(hap)
    if app:
        with zipfile.ZipFile(app) as z:
            haps = [n for n in z.namelist() if n.endswith('.hap')]
            if len(haps) != 1: raise ValueError('Expected one entry HAP in store App')
            with z.open(haps[0]) as stream: check_package(stream)
    result = dict(schemaVersion=1, status='PASS', architecture='ARM64', steamRelease=plan['id'], archiveSetId=archive_set,
                  protonRuntimeId=runtime['id'],
                  packages=[dict(kind='Steam Linux', fileName=steam.name, bytes=metadata['bytes'], sha256=metadata['sha256']),
                            dict(kind='GNU Proton', fileName=tool.name, bytes=proton['bytes'], sha256=proton['sha256'])],
                  checkedPackages=checked,
                  applicationArtifacts=[dict(path=str(path), bytes=path.stat().st_size, sha256=file_hash(path))
                                        for path in (hap, app) if path])
    output.mkdir(parents=True, exist_ok=True)
    (output / REPORT).write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    (output / 'SHA256SUMS-current.txt').write_text(''.join(
        item['sha256'] + '  ' + item['fileName'] + '\n' for item in result['packages']), encoding='utf-8')
    (output / 'OFFLINE-IMPORT.txt').write_text(
        '配套离线组件包\n\n'
        'Steam 组件页 → 导入本地包／更新：选择 ' + steam.name + '\n'
        'GNU Proton 组件页 → 导入此版本 ZIP：选择 ' + tool.name + '\n\n'
        '两份 ZIP 分别导入，不要解压，不要将 SDK 修复包当作完整 Steam 包。\n'
        '导入全程无需联网；安装、更新和修复保留游戏库、账号及存档。\n'
        '仅界面或宿主更新时可复用同一份离线包。官方组件集合或 Proton 载荷变化时，'
        '请使用与应用同时发布的新包；不保证所有历史包跨版本通用。\n'
        '包的大小和 SHA-256 见 release-offline-packages.json / SHA256SUMS-current.txt。\n', encoding='utf-8')
    return result

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--downloads', type=Path, default=Path.home() / '.cache/vpp-steam-offline')
    p.add_argument('--output-dir', type=Path, required=True)
    p.add_argument('--archive-cache', action='append', type=Path, default=[])
    p.add_argument('--offline', action='store_true', help='Require existing exact local component bytes')
    p.add_argument('--verify-only', action='store_true')
    p.add_argument('--hap', type=Path)
    p.add_argument('--app', type=Path)
    a = p.parse_args()
    if not a.verify_only: prepare(ROOT, a.downloads, a.output_dir, a.archive_cache, not a.offline)
    print(json.dumps(verify(ROOT, a.output_dir, a.hap, a.app), ensure_ascii=False))

if __name__ == '__main__': main()
