#!/usr/bin/env python3
"""Complete the pinned GE module closure without replacing historical offline components.

Only missing original PE modules are added to the HAP. Existing package/media
modules are referenced, not redistributed twice. ZIP container compression is a
build detail; the tracked lock pins every uncompressed member and both ABIs.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import stat
import tarfile
import urllib.error
import urllib.parse
import urllib.request
import zipfile
from proton_module_contract import DIRECTORIES, ASSEMBLIES, PREFIX_WINDOWS, complete_module_lock, validate_module_lock, sha_file

ROOT = Path(__file__).resolve().parents[1]
LOCK = Path('docs/linux-steam/gnu-proton-modules.lock.json')
RAW = Path('entry/src/main/resources/rawfile/linux-proton/modules-support')
BASE = Path('docs/linux-steam/gnu-proton-probe.lock.json')
MEDIA = Path('docs/linux-steam/proton-media.lock.json')


def rows_for(root, lock):
    validate_module_lock(lock)
    base = json.loads((root/BASE).read_text())
    media = json.loads((root/MEDIA).read_text())
    if lock['distribution'] != base['distribution'] or media['distribution'] != base['distribution']:
        raise ValueError('Mixed GE module distributions')
    available = {r['path']: ('package/'+r['path'], r) for r in base['selected']}
    available.update({r['path']: ('media/'+r['path'], r) for r in media['files']})
    packaged = {r['path'] for r in base['selected']}
    unix_names = {Path(r['path']).stem for r in lock['selected']
                  if r['path'].startswith(DIRECTORIES[2]+'/') and r['path'].endswith('.so')}
    rows = []
    for r in lock['selected']:
        if r['path'] in ASSEMBLIES:
            rows.append(dict(path=r['path'].removeprefix(PREFIX_WINDOWS+'/'), original=r['path'],
                             source='archive', bytes=r['bytes'], sha256=r['sha256']))
            continue
        if not any(r['path'].startswith(d+'/') for d in (DIRECTORIES[0], DIRECTORIES[1], DIRECTORIES[3], DIRECTORIES[4])):
            continue
        # ICU forwarder targets are shipped in their own original GE directories.
        parts = r['path'].split('/')
        arch, name = parts[-2:]
        is_icu = r['path'].startswith('files/lib/wine/icu/')
        if not name or any(c not in 'abcdefghijklmnopqrstuvwxyz0123456789_.-' for c in name):
            raise ValueError('Unsafe Wine module basename: '+name)
        target = ('system32/' if arch == 'aarch64-windows' else 'syswow64/')
        if name.endswith('.sys') and name != 'win32k.sys':
            target += 'drivers/'
        source, old = available.get(r['path'], ('archive', r))
        if any(old[k] != r[k] for k in ('bytes','sha256')):
            raise ValueError('Existing module source differs: '+name)
        row = dict(path=target+name, original=r['path'], source=source,
                   bytes=r['bytes'], sha256=r['sha256'])
        # Wine only associates a PE builtin with its Unix companion after
        # finding the original PE in its builtin search directory. A plain
        # system32 copy alone falls back without that association (zero Vulkan
        # extensions for winevulkan). Seed just these small companion PEs too.
        if not is_icu and arch == 'aarch64-windows' and Path(name).stem in unix_names and r['path'] not in packaged:
            row['builtin'] = True
        rows.append(row)
    if len({r['path'] for r in rows}) != len(rows):
        raise ValueError('Duplicate prefix module')
    return sorted(rows, key=lambda r:r['path'])


def identity(root, lock):
    base=json.loads((root/BASE).read_text())
    backend=next(r for r in base['selected'] if r['path']=='files/lib/wine/aarch64-unix/winevulkan.so')
    files=rows_for(root,lock)
    result=dict(schemaVersion=1, architecture='aarch64-linux-gnu', distribution=lock['distribution'],
                backend=backend, closure=lock['wineModuleClosure'], files=files,
                installedBytes=sum(r['bytes'] for r in files),
                archiveInstalledBytes=sum(r['bytes'] for r in files if r['source']=='archive'),
                builtinInstalledBytes=sum(r['bytes'] for r in files if r.get('builtin')))
    result['id']=hashlib.sha256(json.dumps(result,sort_keys=True,separators=(',',':')).encode()).hexdigest()
    return result


def verify_contract(root=ROOT):
    return identity(root,json.loads((root/LOCK).read_text()))


def verify(root=ROOT, hap=None):
    lock=json.loads((root/LOCK).read_text())
    expected=identity(root,lock)
    folder=root/RAW
    manifest=json.loads((folder/'manifest.json').read_text())
    archive=manifest.get('archive',{})
    if {k:v for k,v in manifest.items() if k!='archive'} != expected:
        raise ValueError('GNU module manifest/closure mismatch')
    path=folder/'modules.zip'
    if path.is_symlink() or path.stat().st_size != archive.get('bytes') or sha_file(path,'sha256') != archive.get('sha256'):
        raise ValueError('GNU module archive mismatch')
    wanted={r['path']:r for r in expected['files'] if r['source']=='archive'}
    with zipfile.ZipFile(path) as z:
        names=z.namelist()
        if len(names)!=len(set(names)) or set(names)!=set(wanted):
            raise ValueError('GNU module archive inventory mismatch')
        for name in names:
            info=z.getinfo(name)
            if stat.S_ISLNK(info.external_attr>>16) or info.is_dir() or info.file_size != wanted[name]['bytes']:
                raise ValueError('Invalid original GE archive member')
            with z.open(name) as f:
                h=hashlib.sha256()
                for chunk in iter(lambda:f.read(1048576),b''):h.update(chunk)
            if h.hexdigest()!=wanted[name]['sha256']:raise ValueError('GE module checksum: '+name)
    if hap:
        for name in ('manifest.json','modules.zip'):
            with hap.open('resources/rawfile/linux-proton/modules-support/'+name) as f:
                h=hashlib.sha256()
                for chunk in iter(lambda:f.read(1048576),b''):h.update(chunk)
            if h.hexdigest()!=sha_file(folder/name,'sha256'):raise ValueError('HAP GE modules mismatch')
    return manifest


def build(root, archive):
    lock=json.loads((root/LOCK).read_text())
    if complete_module_lock(json.loads((root/BASE).read_text()),archive) != lock:
        raise ValueError('GE archive no longer matches complete directory lock')
    result=identity(root,lock)
    wanted={r['original']:r for r in result['files'] if r['source']=='archive'}
    folder=root/RAW;folder.mkdir(parents=True,exist_ok=True)
    target=folder/'modules.zip';temporary=folder/'modules.zip.pending'
    if target.is_symlink() or temporary.is_symlink():raise ValueError('Linked module output')
    prefix=lock['distribution']['file'].removesuffix('.tar.gz')+'/'
    try:
        with zipfile.ZipFile(temporary,'w',compression=zipfile.ZIP_DEFLATED,compresslevel=6) as out, tarfile.open(archive,'r|gz') as source:
            for member in source:
                row=wanted.pop(member.name.removeprefix(prefix),None) if member.name.startswith(prefix) else None
                if row is None:continue
                if not member.isfile() or member.size!=row['bytes']:raise ValueError('Invalid GE member')
                info=zipfile.ZipInfo(row['path'],(2020,1,1,0,0,0));info.compress_type=zipfile.ZIP_DEFLATED
                info.external_attr=(stat.S_IFREG|0o600)<<16
                h=hashlib.sha256()
                with source.extractfile(member) as src,out.open(info,'w') as dst:
                    for chunk in iter(lambda:src.read(1048576),b''):
                        h.update(chunk);dst.write(chunk)
                if h.hexdigest()!=row['sha256']:raise ValueError('GE original module changed')
        if wanted:raise ValueError('Missing original GE modules')
        os.replace(temporary,target)
    finally:
        temporary.unlink(missing_ok=True)
    result['archive']=dict(bytes=target.stat().st_size,sha256=sha_file(target,'sha256'))
    (folder/'manifest.json').write_text(json.dumps(result,indent=2)+'\n')
    return verify(root)


def open_distribution(dist, repository='GloriousEggroll/proton-ge-custom', opener=None):
    """Use GitHub's asset API if its browser-download gateway is unavailable."""
    opener = opener or urllib.request.urlopen
    prefix = 'https://github.com/' + repository + '/releases/download/'
    if not dist['source'].startswith(prefix):
        raise ValueError('Unexpected distribution URL')
    headers = {'User-Agent': 'VintagePomeloPro-build/1'}
    try:
        return opener(urllib.request.Request(dist['source'], headers=headers), timeout=90)
    except urllib.error.HTTPError as error:
        if error.code not in (429, 500, 502, 503, 504):
            raise
        error.close()
    tag, filename = dist['source'].removeprefix(prefix).split('/')
    if filename != dist['file']:
        raise ValueError('GE source filename differs from distribution')
    api = 'https://api.github.com/repos/' + repository + '/releases/'
    request = urllib.request.Request(api + 'tags/' + urllib.parse.quote(tag, safe=''),
                                     headers={**headers, 'Accept': 'application/vnd.github+json'})
    with opener(request, timeout=90) as response:
        metadata = response.read(1024 * 1024 + 1)
    if len(metadata) > 1024 * 1024:
        raise ValueError('Oversized GE release metadata')
    release = json.loads(metadata)
    assets = [asset for asset in release.get('assets', [])
              if asset.get('name') == filename and asset.get('size') == dist['bytes'] and
              asset.get('browser_download_url') == dist['source']]
    if release.get('tag_name') != tag or len(assets) != 1:
        raise ValueError('GitHub asset differs from pinned GE distribution')
    asset_url = assets[0].get('url', '')
    if not asset_url.startswith(api + 'assets/') or not asset_url.removeprefix(api + 'assets/').isdigit():
        raise ValueError('Unexpected GitHub GE asset API URL')
    return opener(urllib.request.Request(asset_url,
        headers={**headers, 'Accept': 'application/octet-stream'}), timeout=90)


def prepare(root=ROOT, cache=None, verify_only=False, archive=None):
    if verify_only:return verify(root)
    try:return verify(root)
    except (FileNotFoundError,ValueError,zipfile.BadZipFile):pass
    lock=json.loads((root/LOCK).read_text());dist=lock['distribution']
    if archive is None:
        cache=Path(cache or root/'build/linux-runtime-downloads');cache.mkdir(parents=True,exist_ok=True)
        archive=cache/dist['file']
    archive=Path(archive)
    if archive.is_symlink():raise ValueError('Linked GE source archive')
    if not archive.is_file() or archive.stat().st_size!=dist['bytes'] or sha_file(archive,'sha512')!=dist['sha512']:
        url=dist['source']
        if not url.startswith('https://github.com/GloriousEggroll/proton-ge-custom/releases/download/'):
            raise ValueError('Unexpected upstream GE URL')
        pending=archive.with_suffix('.part')
        if pending.is_symlink():raise ValueError('Linked GE download')
        try:
            with open_distribution(dist) as src,pending.open('wb') as dst:
                count=0
                for chunk in iter(lambda:src.read(1048576),b''):
                    count+=len(chunk)
                    if count>dist['bytes']:raise ValueError('Oversized GE source archive')
                    dst.write(chunk)
            if pending.stat().st_size!=dist['bytes'] or sha_file(pending,'sha512')!=dist['sha512']:
                raise ValueError('GE download checksum mismatch')
            os.replace(pending,archive)
        finally:pending.unlink(missing_ok=True)
    return build(root,archive)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--archive',type=Path);p.add_argument('--cache',type=Path);p.add_argument('--verify-only',action='store_true')
    p.add_argument('--refresh-lock',action='store_true')
    a=p.parse_args()
    if a.refresh_lock:
        if not a.archive:p.error('--refresh-lock requires the verified source archive')
        lock=complete_module_lock(json.loads((ROOT/BASE).read_text()),a.archive)
        (ROOT/LOCK).write_text(json.dumps(lock,indent=2)+'\n')
    result=prepare(cache=a.cache,verify_only=a.verify_only,archive=a.archive)
    print(json.dumps(dict(status='PASS',id=result['id'],files=len(result['files']),
        archiveBytes=result['archive']['bytes'],addedBytes=result['archiveInstalledBytes'])))

if __name__=='__main__':main()
