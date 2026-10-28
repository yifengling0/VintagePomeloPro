#!/usr/bin/env python3
"""Restore the media closure from the exact GE archive, independently of old offline ZIPs.

GNU GE 11 uses WineDMO/FFmpeg, not OHOS Wine's GStreamer plugins. Never mix
musl/OHOS DSOs into this glibc closure. Only locked regular files are shipped.
"""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import subprocess
import io
import tarfile

ROOT = Path(__file__).resolve().parents[1]
RAW = Path('entry/src/main/resources/rawfile/linux-proton/media')
LOCK = Path('docs/linux-steam/proton-media.lock.json')
MEDIA_DLLS = ('devenum.dll', 'quartz.dll', 'mf.dll', 'mfplat.dll', 'mfplay.dll',
    'mfreadwrite.dll', 'mfmediaengine.dll', 'mfasfsrcsnk.dll', 'mfh264enc.dll',
    'mfmp4srcsnk.dll', 'mfsrcsnk.dll', 'winedmo.dll', 'wmadmod.dll', 'wmvdecod.dll',
    'wmvcore.dll', 'msdmo.dll', 'mp3dmod.dll', 'msauddecmft.dll', 'msmpeg2vdec.dll',
    'resampledmo.dll', 'dsdmo.dll')
SUPPORT_PACKAGES = (
 ('universe/libv/libva/libva-drm2_2.14.0-1_arm64.deb','9d86246564dd917409a8337f6ad6faa5afa5b0e5a335c711130988ce3cd1f5f7'),
 ('universe/libv/libva/libva-x11-2_2.14.0-1_arm64.deb','5e6743e7f25ac1a756c3846a7e582cea057a78deb1e5982bb8d1c77fd4aedce5'),
 ('main/libv/libvdpau/libvdpau1_1.4-3build2_arm64.deb','2e1e5cfb054095db5e62aa0e567bb4d1247b3b63a0e2be31d48f218c00519336'),
)
MEDIA_LIBS = ('libavcodec.so.62', 'libavformat.so.62', 'libavutil.so.60',
              'libswscale.so.9', 'libswresample.so.6')


def sha(data): return hashlib.sha256(data).hexdigest()


def verify(root=ROOT, hap=None):
    from check_runtime_components import elf_needed
    upstream = json.loads((root/'docs/linux-steam/gnu-proton-probe.lock.json').read_text())
    manifest = json.loads((root/LOCK).read_text())
    if (manifest['schemaVersion'] != 1 or manifest['architecture'] != 'aarch64-linux-gnu' or
            manifest['distribution'] != upstream['distribution'] or
            manifest['requiredDlls'] != list(MEDIA_DLLS) or manifest['requiredLibraries'] != list(MEDIA_LIBS)):
        raise ValueError('GNU media source/capability contract mismatch')
    if manifest['supportPackages'] != [dict(path=n,sha256=h) for n,h in SUPPORT_PACKAGES]:
        raise ValueError('GNU media support package lock mismatch')
    if (root/RAW/'manifest.json').read_bytes() != (root/LOCK).read_bytes():
        raise ValueError('GNU media packaged manifest mismatch')
    deployment=json.loads((root/'entry/src/main/resources/rawfile/linux-steam/deployment.json').read_text())
    shared={Path(row['path']).name:row for row in deployment['files'] if row['path'].startswith('release/runtime/') and row['path'].count('/')==2}
    for name,row in manifest['sharedDependencies'].items():
        if any(row[k]!=shared[name][k] for k in ('bytes','sha256')): raise ValueError('GNU media shared dependency changed: '+name)
    available = {r['path']:r for r in upstream['selected']}
    if manifest['backend'] != available.get('files/lib/wine/aarch64-unix/winedmo.so'):
        raise ValueError('GNU media backend identity mismatch')
    names=set(); libraries={}; prefix=[]
    for row in manifest['files']:
        name=row['path']; path=PurePosixPath(name)
        if path.is_absolute() or '..' in path.parts or '\\' in name or name in names:
            raise ValueError('Unsafe/duplicate GNU media member')
        names.add(name)
        if name in available: raise ValueError('GNU media must not replace the selected upstream: '+name)
        p=root/RAW/name
        if p.is_symlink(): raise ValueError('Linked GNU media input')
        data=p.read_bytes()
        if len(data)!=row['bytes'] or sha(data)!=row['sha256']:
            raise ValueError('GNU media checksum: '+name)
        if '/aarch64-linux-gnu/' in name:
            if data[:6]!=b'\x7fELF\x02\x01' or int.from_bytes(data[18:20],'little')!=183:
                raise ValueError('GNU media ELF architecture: '+name)
            libraries[path.name]=data
        elif '/aarch64-windows/' in name:
            from fex_simd_runtime import pe_kind
            if pe_kind(data) not in ('arm64','arm64ec','arm64x'): raise ValueError('GNU media PE architecture: '+name)
            prefix.append('aarch64-windows/'+path.name)
        elif name not in ('prefix-modules.list','runtime-modules.list') and not name.startswith('licenses/'): raise ValueError('Unexpected GNU media path: '+name)
        if hap:
            if hap.read('resources/rawfile/'+str(RAW.relative_to('entry/src/main/resources/rawfile')/path))!=data:
                raise ValueError('HAP GNU media mismatch: '+name)
    if not set(MEDIA_LIBS)<=libraries.keys(): raise ValueError('Missing GNU FFmpeg library')
    platform={'libc.so.6','libm.so.6','libpthread.so.0','libdl.so.2','librt.so.1','libgcc_s.so.1','libstdc++.so.6','libz.so.1','ld-linux-aarch64.so.1'}
    for name,data in libraries.items():
        missing=set(elf_needed(name,data))-libraries.keys()-platform-manifest['sharedDependencies'].keys()
        if missing: raise ValueError('Missing GNU media dependency: '+str(sorted(missing)))
    for arch in ('aarch64-windows','i386-windows'):
        for name in MEDIA_DLLS:
            key='files/lib/wine/'+arch+'/'+name
            if key not in names and key not in available: raise ValueError('Missing GNU media DLL: '+key)
    if (root/RAW/'runtime-modules.list').read_text()!= ''.join(n+'\n' for n in sorted(manifest['sharedDependencies'])):
        raise ValueError('GNU media shared runtime inventory mismatch')
    if (root/RAW/'prefix-modules.list').read_text()!= ''.join(n+'\n' for n in sorted(prefix)):
        raise ValueError('GNU media prefix inventory mismatch')
    actual={p.relative_to(root/RAW).as_posix() for p in (root/RAW).rglob('*') if p.is_file()}
    if actual != names | {'manifest.json'}:raise ValueError('Unlisted GNU media payload')
    identity={k:v for k,v in manifest.items() if k!='id'}
    if sha(json.dumps(identity,sort_keys=True,separators=(',',':')).encode())!=manifest['id']:
        raise ValueError('GNU media content identity mismatch')
    if hap and hap.read('resources/rawfile/linux-proton/media/manifest.json')!=(root/RAW/'manifest.json').read_bytes():
        raise ValueError('HAP GNU media manifest mismatch')
    return manifest


def prepare(archive, runtime, support_cache, root=ROOT):
    from check_runtime_components import elf_needed
    from prepare_linux_proton_modules import imports
    upstream=json.loads((root/'docs/linux-steam/gnu-proton-probe.lock.json').read_text())
    dist=upstream['distribution']; archive=archive.resolve(strict=True)
    if archive.stat().st_size!=dist['bytes']: raise ValueError('GE media archive size mismatch')
    h=hashlib.sha512()
    with archive.open('rb') as f:
        for b in iter(lambda:f.read(4*1024*1024),b''):h.update(b)
    if h.hexdigest()!=dist['sha512']: raise ValueError('GE media archive checksum mismatch')
    # Cache only metadata; large third-party files are not copied into a new tree.
    blobs={};links={}
    with tarfile.open(archive,'r|gz') as stream:
        for member in stream:
            parts=PurePosixPath(member.name).parts
            if not parts or parts[0]!=dist['file'].removesuffix('.tar.gz'): continue
            name=str(PurePosixPath(*parts[1:]));path=PurePosixPath(name)
            if '..' in path.parts: raise ValueError('Unsafe upstream member')
            if path.parent not in (PurePosixPath('files/lib/aarch64-linux-gnu'),PurePosixPath('files/lib/wine/aarch64-windows')):continue
            if member.issym():
                target=PurePosixPath(member.linkname)
                if target.is_absolute() or len(target.parts)!=1: raise ValueError('Unsafe upstream alias')
                links[name]=str(path.parent/target)
            elif member.isfile() and member.size<=64*1024*1024:
                blobs[name]=stream.extractfile(member).read()
    notices={}
    for relative,expected in SUPPORT_PACKAGES:
        package=support_cache/Path(relative).name
        if sha(package.read_bytes())!=expected: raise ValueError('GNU media support package checksum')
        data=subprocess.check_output(['dpkg-deb','--fsys-tarfile',str(package)])
        with tarfile.open(fileobj=io.BytesIO(data)) as archive_stream:
            for member in archive_stream:
                name=member.name.removeprefix('./')
                if member.isfile() and name.endswith('/copyright'):
                    notices['licenses/'+package.stem+'-copyright.txt']=archive_stream.extractfile(member).read()
                if not name.startswith('usr/lib/aarch64-linux-gnu/'):continue
                key='files/lib/aarch64-linux-gnu/'+Path(name).name
                if member.isfile():blobs[key]=archive_stream.extractfile(member).read()
                elif member.issym():links[key]='files/lib/aarch64-linux-gnu/'+Path(member.linkname).name
    deployment=json.loads((root/'entry/src/main/resources/rawfile/linux-steam/deployment.json').read_text())
    shared_available={Path(row['path']).name:row for row in deployment['files'] if row['path'].startswith('release/runtime/') and row['path'].count('/')==2}
    shared={}
    def use_shared(name):
        if name in shared:return
        row=shared_available[name];data=(runtime/name).read_bytes()
        if len(data)!=row['bytes'] or sha(data)!=row['sha256']:raise ValueError('Unverified shared GNU media dependency: '+name)
        shared[name]=dict(bytes=len(data),sha256=sha(data))
        for dep in elf_needed(name,data):use_shared(dep)
    known={r['path'] for r in upstream['selected']}; chosen={}; sources={}
    def resolve(name):
        seen=set()
        while name in links:
            if name in seen: raise ValueError('Cyclic media alias')
            seen.add(name);name=links[name]
        if name not in blobs: raise ValueError('Missing upstream media member: '+name)
        return name
    pending=['files/lib/wine/aarch64-windows/'+n for n in MEDIA_DLLS]
    seen=set()
    while pending:
        name=pending.pop()
        if name in seen:continue
        seen.add(name)
        if name in known:continue
        # Wine API sets already resolve through the selected kernelbase closure.
        if PurePosixPath(name).name.startswith(('api-ms-','ext-ms-')):continue
        source=resolve(name);data=blobs[source]
        chosen[name]=data;sources[name]=source
        pending.extend('files/lib/wine/aarch64-windows/'+n for n in imports(data))
    pending=['files/lib/aarch64-linux-gnu/'+n for n in MEDIA_LIBS]; seen=set()
    while pending:
        name=pending.pop()
        if name in seen:continue
        seen.add(name);source=resolve(name);data=blobs[source]
        chosen[name]=data;sources[name]=source
        for dependency in elf_needed(name,data):
            target='files/lib/aarch64-linux-gnu/'+dependency
            if dependency in shared_available:use_shared(dependency)
            elif target in blobs or target in links:pending.append(target)
            elif dependency not in {'libc.so.6','libm.so.6','libpthread.so.0','libdl.so.2','librt.so.1','libgcc_s.so.1','libstdc++.so.6','libz.so.1','ld-linux-aarch64.so.1'}:
                raise ValueError('Unresolved upstream media dependency: '+dependency)
    chosen.update(notices)
    chosen['runtime-modules.list']=''.join(n+'\n' for n in sorted(shared)).encode()
    prefix=sorted('aarch64-windows/'+PurePosixPath(n).name for n in chosen if '/aarch64-windows/' in n)
    chosen['prefix-modules.list']=''.join(n+'\n' for n in prefix).encode()
    records=[]
    for name,data in sorted(chosen.items()):
        p=root/RAW/name;p.parent.mkdir(parents=True,exist_ok=True)
        if p.is_symlink():raise ValueError('Linked media destination')
        if not p.is_file() or p.read_bytes()!=data:p.write_bytes(data)
        records.append(dict(path=name,bytes=len(data),sha256=sha(data),source=sources.get(name,'generated:media-dll-closure')))
    # Keep a prerequisite fingerprint so an independently selected future GE
    # backend cannot silently load these FFmpeg major versions with another ABI.
    backend=next(r for r in upstream['selected'] if r['path']=='files/lib/wine/aarch64-unix/winedmo.so')
    manifest=dict(schemaVersion=1,architecture='aarch64-linux-gnu',distribution=dist,
        requiredDlls=list(MEDIA_DLLS),requiredLibraries=list(MEDIA_LIBS),backend=backend,
        files=records,sharedDependencies=shared,supportPackages=[dict(path=n,sha256=h) for n,h in SUPPORT_PACKAGES],installedBytes=sum(r['bytes'] for r in records))
    manifest['id']=sha(json.dumps(manifest,sort_keys=True,separators=(',',':')).encode())
    data=(json.dumps(manifest,indent=2)+'\n').encode()
    (root/LOCK).write_bytes(data);(root/RAW/'manifest.json').write_bytes(data)
    verify(root)
    return manifest


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--archive',type=Path);p.add_argument('--verify-only',action='store_true');p.add_argument('--refresh',action='store_true');p.add_argument('--runtime',type=Path);p.add_argument('--support-cache',type=Path);a=p.parse_args()
    try:
        if a.refresh:raise ValueError('Requested closure refresh')
        m=verify()
    except (OSError,ValueError,KeyError):
        if a.verify_only or not a.archive:raise
        m=prepare(a.archive,a.runtime,a.support_cache)
    print('GNU media closure:',len(m['files']),'files,',m['installedBytes'],'bytes; verified')

if __name__=='__main__':main()
