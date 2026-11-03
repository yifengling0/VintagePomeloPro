#!/usr/bin/env python3
"""Build GNU Venus overlays without mutating the shared Mesa tree or objects.

The bo_ops layout changes: rebuild EVERY Venus translation unit and relink with
private objects. Non-Venus static dependencies remain shared, immutable inputs.
"""
import argparse, hashlib, json, os, shlex, shutil, subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
PATCHES=ROOT/'patches/linux-proton/mesa'
BASE_PATCHES=ROOT/'patches/linux-proton/mesa-base'
def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()
def prepare(source, target):
    paths=sorted((source/'src/virtio/vulkan').glob('*.[ch]'))
    paths += [source/'src/virtio/vtest/vtest_protocol.h', source/'src/vulkan/vulkan.sym']
    paths += sorted((source/'include/vulkan').glob('*.h'))
    paths += sorted((source/'include/vk_video').glob('*.h'))
    # Refresh this small source overlay; never duplicate a Mesa build/rootfs.
    for path in paths:
        dest=target/path.relative_to(source); dest.parent.mkdir(parents=True,exist_ok=True)
        shutil.copyfile(path,dest)
    baseline=json.loads((ROOT/'docs/linux-steam/gnu-venus-base.lock.json').read_text())['files']
    if {str(p.relative_to(source)):sha(p) for p in paths}!=baseline:
        for patch in sorted(BASE_PATCHES.glob('*.patch')):
            subprocess.run(['patch','--batch','--fuzz=0','-p1','-i',str(patch)],cwd=target,check=True,stdout=subprocess.DEVNULL)
    effective={str(p.relative_to(source)):sha(target/p.relative_to(source)) for p in paths}
    if effective!=baseline:raise ValueError('GNU Mesa baseline changed; review source contract')
    for patch in sorted(PATCHES.glob('*.patch')):
        subprocess.run(['patch','--batch','--fuzz=0','-p1','-i',str(patch)],cwd=target,check=True,stdout=subprocess.DEVNULL)
    return effective
def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ['source','output']:p.add_argument('--'+name,type=Path,required=True)
    for name in ['build','meson-tools']:p.add_argument('--'+name,type=Path)
    p.add_argument('--prepare-only',action='store_true'); a=p.parse_args()
    source=a.source.resolve(strict=True); out=a.output.resolve(); out.mkdir(parents=True,exist_ok=True)
    if out.is_relative_to(source): p.error('Keep overlays outside shared Mesa')
    overlay=out/'source'; base=prepare(source,overlay)
    baseline=ROOT/'docs/linux-steam/gnu-venus-base.lock.json'
    if baseline.exists() and json.loads(baseline.read_text())['files']!=base:raise ValueError('GNU Mesa baseline changed; review source contract')
    if a.prepare_only: print('Prepared GNU Venus overlays:',overlay);return
    if not a.build or not a.meson_tools:p.error('--build and --meson-tools required')
    build=a.build.resolve(strict=True)
    rows=json.loads((build/'compile_commands.json').read_text())
    rows=[r for r in rows if r['output'].startswith('src/virtio/vulkan/libvulkan_virtio.so.p/')]
    if not rows: raise ValueError('No Venus translation units')
    commands=[]; replacements={}
    for row in rows:
        command=shlex.split(row['command']); src=(build/row['file']).resolve(strict=True)
        if src.is_relative_to(source):dest=overlay/src.relative_to(source)
        elif src.is_relative_to(build/'src/virtio/vulkan'):
            dest=overlay/src.relative_to(build); shutil.copyfile(src,dest)
        elif base.get('src/virtio/vulkan/'+src.name)==sha(src):
            # An existing verified GNU Meson configuration can reference its
            # older materialized source root. Compile the reviewed overlay,
            # never that tree's potentially different bo_ops header layout.
            dest=overlay/'src/virtio/vulkan'/src.name
        else:raise ValueError('Unexpected generated Venus source: '+str(src))
        if not dest.is_file(): raise ValueError('Venus source outside overlay: '+str(src))
        command[command.index(row['file'])]=str(dest)
        obj=out/Path(row['output']).name; replacements[row['output']]=str(obj)
        for flag,value in [('-o',str(obj)),('-MQ',str(obj)),('-MF',str(obj)+'.d')]:
            command[command.index(flag)+1]=value
        command[1:1]=['-I'+str(overlay/'src'),'-I'+str(overlay/'src/virtio'),'-I'+str(overlay/'src/virtio/vulkan')]
        commands.append(command)
    env={**os.environ,'PATH':str(a.meson_tools.resolve(strict=True))+':'+os.environ['PATH']}
    steps=subprocess.check_output(['ninja','-C',str(build),'-t','commands','src/virtio/vulkan/libvulkan_virtio.so'],env=env,text=True).splitlines()
    link=shlex.split(steps[-1]); deps={}
    for i,token in enumerate(link):
        if token in replacements:link[i]=replacements[token]
        elif token.endswith(('.a','.so','.o')) and (build/token).is_file():deps[token]=sha(build/token)
    binary=out/'libvulkan_virtio.so'; pending=out/'libvulkan_virtio.pending.so'
    link[link.index('-o')+1]=str(pending)
    link[link.index('-Wl,--version-script')+1]=str(overlay/'src/vulkan/vulkan.sym')
    inputs={'script':sha(Path(__file__)),'base':base,'patches':{str(x.relative_to(ROOT)):sha(x) for x in [*sorted(BASE_PATCHES.glob('*.patch')),*sorted(PATCHES.glob('*.patch'))]},
        'commands':commands,'link':link,'dependencies':deps,'generated':{str(x.relative_to(build)):sha(x) for x in sorted((build/'src/virtio/vulkan').rglob('*.h'))},
        'compiler':sha(Path(commands[0][0]))}
    receipt=out/'venus-alias-build.json'
    if receipt.is_file() and binary.is_file():
        old=json.loads(receipt.read_text())
        if old.get('inputs')==inputs and old.get('sha256')==sha(binary):print('Reused complete GNU Venus overlay build');return
    with (out/'venus-alias-build.log').open('w') as log:
        for command in commands:subprocess.run(command,cwd=build,env=env,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=180)
        subprocess.run(link,cwd=build,env=env,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=180)
    symbols=subprocess.check_output(['readelf','--dyn-syms','--wide',str(pending)],text=True)
    for symbol in ['vn_winehua_mapped_memory_fd','vn_winehua_present','vk_icdGetInstanceProcAddr']:
        if symbol not in symbols:raise ValueError('Missing GNU Venus entry: '+symbol)
    pending.replace(binary)
    receipt.write_text(json.dumps({'inputs':inputs,'translationUnits':len(rows),'bytes':binary.stat().st_size,'sha256':sha(binary)},indent=2)+'\n')
    print('Built complete GNU Venus overlay:',len(rows),'units,',binary.stat().st_size,'bytes')
if __name__=='__main__':main()
