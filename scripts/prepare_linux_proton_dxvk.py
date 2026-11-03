#!/usr/bin/env python3
"""Verify/install the independent GNU DXVK support payload for normal HAP builds."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import shutil
import tarfile

ROOT = Path(__file__).resolve().parents[1]
RAW = Path('entry/src/main/resources/rawfile/linux-proton/dxvk-support')
LOCK = Path('docs/linux-steam/gnu-dxvk-support.lock.json')
PROFILE = {
    'VPP_GNU_DXVK_PROFILE': 'legacy-1.10.3',
    'WINEHUA_DXVK_RELAXED_FEATURES': '1',
    'DXVK_WINEHUA_COMMAND_QUERY_RESET': '1',
    'DXVK_WINEHUA_FLUSH_DYNAMIC_MAPPED': '1',
    'DXVK_WINEHUA_EMULATE_RGBA8_SNORM_RT': 'auto',
    'DXVK_WINEHUA_PRECISE_SHADOW': '1',
    'DXVK_WINEHUA_BATCH_MAPPED_FLUSH': '1',
    'VN_OHOS_PRECISE_SHADOW': '1',
    'VN_WINEHUA_PERSISTENT_MAP_SYNC': '0',
}
NATIVE = ('libproton_probe.so', 'libproton_vulkan.so', 'libvulkan_virtio.so')
BRIDGE = ('vulkan-1.dll', 'winevulkan.dll', 'd3dcompiler_47.dll')

def sha(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for b in iter(lambda: f.read(1048576), b''): h.update(b)
    return h.hexdigest()

def sources(root):
    fixture = root/'entry/src/main/cpp/linux_steam/gnu_fixture'
    paths = [root/'scripts'/n for n in ('prepare_linux_proton_dxvk.py',
        'build_linux_proton_probe.py', 'build_linux_proton_venus.py', 'build_linux_proton_vulkan.py')]
    paths += list((root/'patches/linux-proton/mesa').glob('*.patch')) + list((root/'patches/linux-proton/mesa-base').glob('*.patch'))
    paths += list(fixture.glob('*.h')) + list(fixture.parent.glob('*.h'))
    paths += [fixture/n for n in ('exec_compat.cpp', 'filesystem_compat.c', 'posix_compat.c',
        'sysvipc_compat.c', 'popen_compat.c', 'socket_metadata.c', 'syscall_compat.S', 'proton_vulkan_wsi.cpp')]
    paths += [fixture.parent/n for n in ('native_loader.cpp', 'native_loader_entry.S')]
    paths += [root/'docs/linux-steam/gnu-dxvk-source.lock.json', root/'docs/linux-steam/gnu-venus-base.lock.json']
    return {p.relative_to(root).as_posix(): sha(p) for p in sorted(set(paths))}

def verify(root=ROOT, hap=None):
    from dxvk_pe import validate_dxvk, inspect_pe
    m = json.loads((root/LOCK).read_text())
    upstream = json.loads((root/'docs/linux-steam/gnu-proton-probe.lock.json').read_text())
    source_lock = json.loads((root/'docs/linux-steam/gnu-dxvk-source.lock.json').read_text())
    backend = next(r for r in upstream['selected'] if r['path']=='files/lib/wine/aarch64-unix/winevulkan.so')
    if (m['schemaVersion']!=1 or m['architecture']!='aarch64-linux-gnu' or m['profile']!=PROFILE or
            m['backend']!=backend or m['distribution']!=upstream['distribution'] or m['sources']!=sources(root)):
        raise ValueError('GNU DXVK source/profile/backend mismatch; rebuild companion')
    if (root/RAW/'manifest.json').read_bytes()!=(root/LOCK).read_bytes():
        raise ValueError('GNU DXVK packaged manifest mismatch')
    if hap and hap.read('resources/rawfile/linux-proton/dxvk-support/manifest.json')!=(root/LOCK).read_bytes():
        raise ValueError('HAP GNU DXVK manifest mismatch')
    expected=set(NATIVE)|{'LICENSE.dxvk'}|{'pe/'+n for n in BRIDGE}
    expected|={'dlls/'+a+'/'+n for a in ('arm64x','i386') for n in ('d3d11.dll','dxgi.dll')}
    names=set()
    for row in m['files']:
        name=row['path']
        if name not in expected or name in names: raise ValueError('Unexpected GNU DXVK member')
        names.add(name);path=root/RAW/name
        if path.is_symlink() or path.stat().st_size!=row['bytes'] or sha(path)!=row['sha256']:
            raise ValueError('GNU DXVK payload mismatch: '+name)
        data=path.read_bytes()
        if name in NATIVE:
            if data[:6]!=b'\x7fELF\x02\x01' or int.from_bytes(data[18:20],'little')!=183:
                raise ValueError('Non-GNU ARM64 support object: '+name)
        elif name.startswith('dlls/'):
            _,arch,dll=name.split('/')
            pin=source_lock['variants']['legacy-1.10.3']['architectures'][arch]['files'][dll]
            if any(row[k]!=pin[k] for k in ('bytes','sha256')): raise ValueError('Unpinned DXVK fork DLL')
            validate_dxvk(data, dll, arch)
        elif name.startswith('pe/'):
            pe=inspect_pe(data)
            if pe['architecture']!='arm64x': raise ValueError('GE bridge lacks both ARM64X views: '+name)
            if row.get('source')!='files/lib/wine/aarch64-windows/'+PurePosixPath(name).name:
                raise ValueError('GE PE/Unix bridge source mismatch')
        if hap and hap.read('resources/rawfile/linux-proton/dxvk-support/'+name)!=data:
            raise ValueError('HAP GNU DXVK payload mismatch')
    if names!=expected or sum(r['bytes'] for r in m['files'])!=m['installedBytes']:
        raise ValueError('GNU DXVK payload closure mismatch')
    actual={p.relative_to(root/RAW).as_posix() for p in (root/RAW).rglob('*') if p.is_file()}
    if actual!=expected|{'manifest.json'}:raise ValueError('Unlisted GNU DXVK file')
    identity={k:v for k,v in m.items() if k!='id'}
    if hashlib.sha256(json.dumps(identity,sort_keys=True,separators=(',',':')).encode()).hexdigest()!=m['id']:
        raise ValueError('GNU DXVK identity mismatch')
    return m

def prepare(args):
    from dxvk_pe import validate_dxvk
    upstream=json.loads((ROOT/'docs/linux-steam/gnu-proton-probe.lock.json').read_text())
    source_lock=json.loads((ROOT/'docs/linux-steam/gnu-dxvk-source.lock.json').read_text())
    dist=upstream['distribution']
    if args.archive.stat().st_size!=dist['bytes']:
        raise ValueError('GE archive size mismatch')
    h=hashlib.sha512()
    with args.archive.open('rb') as f:
        for b in iter(lambda:f.read(1048576),b''):h.update(b)
    if h.hexdigest()!=dist['sha512']:raise ValueError('GE archive hash mismatch')
    chosen={}; origins={}
    for name,folder in [('libproton_probe.so',args.shim),('libproton_vulkan.so',args.wsi),('libvulkan_virtio.so',args.venus)]:
        chosen[name]=folder/name
    for arch in ('arm64x','i386'):
        for dll in ('d3d11.dll','dxgi.dll'):
            source=args.dxvk/arch/'bin'/dll
            pin=source_lock['variants']['legacy-1.10.3']['architectures'][arch]['files'][dll]
            if source.stat().st_size!=pin['bytes'] or sha(source)!=pin['sha256']:raise ValueError('DXVK source-built identity mismatch')
            validate_dxvk(source.read_bytes(),dll,arch)
            chosen['dlls/'+arch+'/'+dll]=source
    chosen['LICENSE.dxvk']=args.license
    if sha(args.license)!=source_lock['variants']['legacy-1.10.3']['license']['sha256']:raise ValueError('DXVK license mismatch')
    dest=ROOT/RAW;dest.mkdir(parents=True,exist_ok=True)
    for name,source in chosen.items():
        p=dest/name;p.parent.mkdir(parents=True,exist_ok=True)
        if p.is_symlink():raise ValueError('Linked GNU DXVK output')
        if not p.is_file() or sha(p)!=sha(source):shutil.copyfile(source,p)
    prefix=dist['file'].removesuffix('.tar.gz')+'/'
    wanted={prefix+'files/lib/wine/aarch64-windows/'+n:n for n in BRIDGE}
    with tarfile.open(args.archive,'r|gz') as stream:
        for member in stream:
            name=member.name.removeprefix('./')
            if name not in wanted:continue
            if not member.isfile() or not 0<member.size<32*1048576:raise ValueError('Invalid GE bridge member')
            relative='pe/'+wanted.pop(name);p=dest/relative;p.parent.mkdir(parents=True,exist_ok=True)
            if p.is_symlink():raise ValueError('Linked GE bridge output')
            data=stream.extractfile(member).read()
            if not p.is_file() or p.read_bytes()!=data:p.write_bytes(data)
            chosen[relative]=p;origins[relative]=name[len(prefix):]
    if wanted:raise ValueError('Missing original GE Vulkan bridge')
    files=[dict(path=n,bytes=(dest/n).stat().st_size,sha256=sha(dest/n),**({'source':origins[n]} if n in origins else {})) for n in sorted(chosen)]
    backend=next(r for r in upstream['selected'] if r['path']=='files/lib/wine/aarch64-unix/winevulkan.so')
    m=dict(schemaVersion=1,architecture='aarch64-linux-gnu',profile=PROFILE,backend=backend,distribution=dist,
        sources=sources(ROOT),files=files,installedBytes=sum(r['bytes'] for r in files),
        deviceValidation='pending VPP device validation; HSL evidence does not qualify VPP')
    m['id']=hashlib.sha256(json.dumps(m,sort_keys=True,separators=(',',':')).encode()).hexdigest()
    text=json.dumps(m,indent=2)+'\n';(ROOT/LOCK).write_text(text);(dest/'manifest.json').write_text(text)
    return verify()

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--refresh',action='store_true');p.add_argument('--verify-only',action='store_true')
    for n in ('archive','dxvk','shim','wsi','venus','license'):p.add_argument('--'+n,type=Path)
    a=p.parse_args()
    if a.refresh:
        if a.verify_only or not all(getattr(a,n) for n in ('archive','dxvk','shim','wsi','venus','license')):p.error('Refresh requires all verified build inputs')
        m=prepare(a)
    else:m=verify()
    print('Verified GNU DXVK support:',len(m['files']),'files,',m['installedBytes'],'bytes')
if __name__=='__main__':main()
