#!/usr/bin/env python3
"""Build the deterministic GNU Proton component from verified reusable outputs.

No Steam client, rootfs, games, user prefix, accounts, or diagnostic markers.
The distribution lock and each adapter build remain the source of truth.
"""
import argparse, hashlib, json, os, re, subprocess, tarfile, zipfile
from pathlib import Path, PurePosixPath
ROOT=Path(__file__).resolve().parents[1]
def digest(p):
    h=hashlib.sha256()
    with p.open('rb') as f:
        for block in iter(lambda:f.read(1024*1024),b''):h.update(block)
    return h.hexdigest()

def validate_native_closure(files, runtime):
    """Reject missing graphics/adapter DT_NEEDED libraries before delivery.

    Only the declared reused runtime files may satisfy external dependencies;
    a developer's extra staged device libraries must never hide missing files.
    """
    reused=(files['runtime-modules.list']).read_text().splitlines()
    reused += ['ld-linux-aarch64.so.1','libc.so.6','libdl.so.2','libpthread.so.0',
               'librt.so.1','libm.so.6','libstdc++.so.6','libgcc_s.so.1',
               'libsteamohos_posix.so','libfreetype.so.6','libfontconfig.so.1',
               'libexpat.so.1','libpng16.so.16','libbrotlidec.so.1','libbrotlicommon.so.1','libz.so.1']
    available={}
    roots=[]
    for name in reused:
        candidates=[runtime/name,runtime/'steamrtarm64'/name]
        source=next((candidate for candidate in candidates if candidate.is_file()),None)
        if source is None:raise ValueError('Missing declared GNU runtime dependency: '+name)
        available[name]=source
    # Runtime files are copied into root; private graphics and package libraries
    # are explicit library-path entries. Component files take lookup precedence.
    for name,source in files.items():
        with source.open('rb') as stream:header=stream.read(20)
        if header[:4]!=b'\x7fELF':continue
        if header[4]!=2 or int.from_bytes(header[18:20],'little')!=183:
            raise ValueError('Expected GNU AArch64 ELF: '+name)
        available[PurePosixPath(name).name]=source
        # Wine's optional Unix modules (USB/pcap/VR/TTS/vendor GPU APIs) are
        # not loaded by this transport. Its selected PE closure and host
        # capability limitations are maintained separately; do not confuse
        # those optional backends with a missing graphics dependency.
        if not name.startswith('package/'):
            roots.append(source)
    queue=roots;seen=set()
    while queue:
        source=queue.pop()
        if source in seen:continue
        seen.add(source)
        text=subprocess.check_output(['readelf','-d',str(source)],text=True)
        for name in re.findall(r'\(NEEDED\).*?\[(.*?)\]',text):
            dependency=available.get(name)
            if dependency is None:
                raise ValueError('Undelivered GNU dependency '+name+' required by '+str(source))
            queue.append(dependency)
    return len(seen)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--inputs',type=Path,required=True)
    p.add_argument('--wine-source',type=Path,required=True)
    p.add_argument('--runtime',type=Path,required=True,help='Verified shared Linux Steam GNU runtime')
    p.add_argument('--output',type=Path,default=ROOT/'entry/src/main/resources/rawfile/linux-proton')
    a=p.parse_args(); b=a.inputs.resolve(); files={};rows=[]
    def add(source,name,expected=None):
        source=Path(source);path=PurePosixPath(name)
        if path.is_absolute() or '..' in path.parts or '\\' in name or name in files:raise ValueError(name)
        if source.is_symlink() or not source.is_file():raise ValueError('Missing/linked input: '+str(source))
        h=digest(source);size=source.stat().st_size
        if expected and (h!=expected['sha256'] or size!=expected['bytes']):raise ValueError('Input changed: '+name)
        with source.open('rb') as f: mode=0o700 if f.read(4)==b'\x7fELF' else 0o600
        files[name]=source;rows.append(dict(path=name,bytes=size,sha256=h,mode=mode))
    lock=json.loads((ROOT/'docs/linux-steam/gnu-proton-probe.lock.json').read_text())
    package=b/'probe-package'
    for row in lock['selected']:add(package/row['path'],'package/'+row['path'],row)
    for name,target in lock['aliases'].items():add(package/name,'package/'+str(PurePosixPath(name).parent/target))
    # INF files and shared data are needed for truly empty prefix initialization.
    for row in json.loads((b/'inf-resources.json').read_text()):
        if 'package/'+row['path'] not in files:add(package/row['path'],'package/'+row['path'],row)
    for name in ['libproton_probe.so','hmosdeck','proton-steam-ready','libSDL2-2.0.so.0','runtime-modules.list','prefix-modules.list','audio-direct','manual-launch.version']:
        add(b/name,name)
    add(b/'direct-audio/libwineohos-gnu.so','libwineohos-gnu.so')
    # Use the complete verified graphics build, including its dependency
    # closure. A hardcoded list used to omit XCB/GLX DSOs and activation then
    # removed the previously working copies from the managed graphics tree.
    graphics=json.loads((b/'proton-graphics-build.json').read_text())['files']
    aliases={'Xwayland-glx':'Xwayland','libGL-proton.so.1':'libGL.so.1'}
    for name,row in graphics.items():
        add(b/name,'graphics/'+aliases.get(name,name),row)
    for name in ['libproton_egl.so','libwayland-egl.so.1','libproton_vulkan.so','libvulkan_virtio.so']:
        add(b/name,'graphics/'+name)
    add(b/'proton-x86-locale.exe','prefix/drive_c/proton-x86-locale.exe')
    for name in ['d3d11.dll','dxgi.dll']:add(b/'dxvk26'/name,'prefix/drive_c/dxvk26/'+name)
    for name in ['marlett.ttf','tahoma.ttf','tahomabd.ttf']:add(a.wine_source/'fonts'/name,'fonts/'+name)
    for source in sorted((b/'locale').rglob('*')):
        if source.is_file():add(source,'locale/'+source.relative_to(b/'locale').as_posix())
    # Preserve upstream licensing and a reproducible source reference.
    add(a.wine_source/'COPYING.LIB','licenses/Wine-COPYING.LIB')
    notices=b/'component-notices.txt'
    notices.write_text('HmosDeck GNU Proton component\nGE-Proton11-11 aarch64: '+lock['distribution']['source']+'\n'
        'Upstream source: https://github.com/GloriousEggroll/proton-ge-custom/tree/GE-Proton11-11\n'
        'Wine source and fonts: https://gitlab.winehq.org/wine/wine\n'
        'Mesa: https://gitlab.freedesktop.org/mesa/mesa (MIT and upstream component licenses)\n'
        'DXVK: https://github.com/doitsujin/dxvk (zlib license)\n'
        'glibc locale data: https://sourceware.org/glibc/ (LGPL-2.1-or-later)\n'
        'SDL2: https://github.com/libsdl-org/SDL/tree/SDL2 (zlib license)\n'
        'OHOS adapter source and build recipes are part of VintagePomeloPro.\n',encoding='utf-8')
    add(notices,'licenses/NOTICE.txt')
    # Licenses bundled by the exact verified upstream archive.
    archive=b/lock['distribution']['file']
    if archive.stat().st_size!=lock['distribution']['bytes']:raise ValueError('GE archive size')
    license_dir=b/'component-licenses';license_dir.mkdir(exist_ok=True)
    with tarfile.open(archive,'r|gz') as stream:
        for member in stream:
            rel=PurePosixPath(*PurePosixPath(member.name).parts[1:])
            if not member.isfile() or member.size>2*1024*1024:continue
            if rel.name.lower().startswith(('license','copying')) and '..' not in rel.parts:
                target=license_dir/hashlib.sha256(str(rel).encode()).hexdigest()
                data=stream.extractfile(member).read()
                if not target.exists() or target.read_bytes()!=data:target.write_bytes(data)
                add(target,'licenses/upstream/'+str(rel))
    native_count=validate_native_closure(files,a.runtime.resolve(strict=True))
    print('Verified delivered/reused GNU ELF dependency closure:',native_count,'files')
    rows.sort(key=lambda r:r['path'])
    identity=dict(schemaVersion=1,distribution=lock['distribution'],files=rows)
    version=hashlib.sha256(json.dumps(identity,sort_keys=True,separators=(',',':')).encode()).hexdigest()
    a.output.mkdir(parents=True,exist_ok=True)
    archive=a.output/'gnu-proton.zip'; manifest=a.output/'manifest.json'
    old=json.loads(manifest.read_text()) if manifest.exists() else {}
    if old.get('id')==version and archive.exists() and digest(archive)==old['sha256']:
        print('Reused verified GNU component',version,archive.stat().st_size);return
    temporary=archive.with_suffix('.part')
    with zipfile.ZipFile(temporary,'w',compression=zipfile.ZIP_DEFLATED,compresslevel=6) as z:
        for row in rows:
            info=zipfile.ZipInfo(row['path'],(2026,1,1,0,0,0));info.compress_type=zipfile.ZIP_DEFLATED
            info.external_attr=(0o100000|row['mode'])<<16
            with z.open(info,'w') as out,files[row['path']].open('rb') as source:
                while chunk:=source.read(1024*1024):out.write(chunk)
    with zipfile.ZipFile(temporary) as z:
        if z.testzip():raise ValueError('ZIP verification failed')
    os.replace(temporary,archive)
    manifest.write_text(json.dumps(dict(**identity,id=version,bytes=archive.stat().st_size,sha256=digest(archive),
        installedBytes=sum(r['bytes'] for r in rows)),indent=2)+'\n')
    print(json.dumps(dict(id=version,files=len(rows),compressed=archive.stat().st_size,installed=sum(r['bytes'] for r in rows))))
if __name__=='__main__':main()
