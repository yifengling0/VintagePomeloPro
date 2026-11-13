#!/usr/bin/env python3
"""Package reviewed native-game repairs beside immutable Steam/Proton archives.

Reuses explicit existing GNU build outputs. Never edits games or Valve depots.
FEX includes the published sigreturn diff and a scoped OHOS guest-VMA adapter.
"""
import argparse, hashlib, json, re, shutil
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
RELATIVE='linux-steam/bootstrap/native-game-support'
GL_NAMES=['libGL-host32.so','libGL-host64.so','libGL-guest32.so','libGL-guest64.so']
NAMES=['FEX','FEXServer','libvulkan_virtio.so','libsteamohos_software_gl.so',*GL_NAMES]
SOURCE_NAMES=['scripts/patches/fex-reproducible-thunkgen.patch','scripts/linux_component_identity.py','scripts/prepare_linux_fex_tooling.py','docs/linux-steam/fex-gl-tooling.lock.json','scripts/prepare_linux_native_repairs.py','scripts/build_linux_native_fex.py','scripts/build_linux_native_gl_thunks.py',
 'scripts/patches/fex-valve-2607-game-contract.patch','scripts/patches/fex-valve-2607-game-contract.provenance.json',
 'scripts/patches/fex-upstream-5901-restore-sigreturn-state.patch',
 'scripts/patches/fex-upstream-5901-restore-sigreturn-state.provenance.json',
 'scripts/build_linux_proton_venus_legacy.py','scripts/build_linux_steam_software_gl.py',
 'scripts/build_linux_steam_client.py','patches/mesa/0011-softpipe-demand-zero-texture-cache.patch',
 'patches/virglrenderer/0007-ohos-native-memory-context.patch',
 'patches/virglrenderer/0008-ohos-native-resource-contract.patch',
 'entry/src/main/cpp/linux_steam/gnu_fixture/native_fex_core.h',
 'entry/src/main/resources/rawfile/'+RELATIVE+'/NOTICE.txt',
 'scripts/patches/fex-ohos-guest-protection.patch',
 'scripts/patches/fex-ohos-guest-protection.h']
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def verify(root=ROOT,hap=None):
 directory=root/'entry/src/main/resources/rawfile'/RELATIVE
 manifest=directory/'manifest.json';v=json.loads(manifest.read_text())
 if (v.get('schemaVersion')!=1 or v.get('architecture')!='aarch64-linux-gnu' or
     set(v.get('files',{}))!=set(NAMES) or set(v.get('sources',{}))!=set(SOURCE_NAMES)):
  raise ValueError('Native repair inventory mismatch')
 for name,expected in v['sources'].items():
  if sha(root/name)!=expected:raise ValueError('Native repair source changed: '+name)
 for name,row in v['files'].items():
  p=directory/name
  if p.is_symlink() or p.stat().st_size!=row['bytes'] or sha(p)!=row['sha256']:
   raise ValueError('Native repair payload mismatch: '+name)
  with p.open('rb') as f:data=f.read(20)
  machine=3 if name=='libGL-guest32.so' else 62 if name=='libGL-guest64.so' else 183
  elfclass=1 if machine==3 else 2
  if data[:6]!=b'\x7fELF'+bytes([elfclass,1]) or int.from_bytes(data[18:20],'little')!=machine:raise ValueError('Native repair ELF mismatch')
  if hap:
   member='resources/rawfile/'+RELATIVE+'/'+name
   with hap.open(member) as stream:
    digest=hashlib.sha256()
    for block in iter(lambda:stream.read(1024*1024),b''):digest.update(block)
   if hap.getinfo(member).file_size!=row['bytes'] or digest.hexdigest()!=row['sha256']:
    raise ValueError('HAP native repair mismatch: '+name)
 # A new toolchain can change the built pair. Fail packaging rather than
 # silently shipping a pair that the compiled selector will never enable.
 selector=re.sub(r'\s+','',(root/'entry/src/main/cpp/linux_steam/gnu_fixture/native_fex_core.h').read_text())
 for name in ['FEX','FEXServer',*GL_NAMES]:
  expected='FileSha256(support+"/'+name+'")!="'+v['files'][name]['sha256']+'"'
  if expected not in selector:raise ValueError('Native repair selector identity mismatch: '+name)
 notice=directory/'NOTICE.txt'
 if not notice.is_file() or not notice.read_text().strip():raise ValueError('Native repair notices missing')
 if hap and hap.read('resources/rawfile/'+RELATIVE+'/NOTICE.txt')!=notice.read_bytes():
  raise ValueError('HAP native repair notices mismatch')
 if hap and hap.read('resources/rawfile/'+RELATIVE+'/manifest.json')!=manifest.read_bytes():
  raise ValueError('HAP native repair manifest mismatch')
 return v
def main():
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument('--verify-only',action='store_true')
 for n in ['fex-build','venus','software-gl','gl-thunks']:p.add_argument('--'+n,type=Path)
 a=p.parse_args();directory=ROOT/'entry/src/main/resources/rawfile'/RELATIVE
 if not a.verify_only:
  if not all([a.fex_build,a.venus,a.software_gl,a.gl_thunks]):p.error('Explicit previously built inputs required')
  receipt=json.loads((a.fex_build/'sigreturn-manifest.json').read_text())
  patch=sha(ROOT/'scripts/patches/fex-upstream-5901-restore-sigreturn-state.patch')
  if receipt.get('patchSha256') or receipt.get('upstreamSigreturnPatchSha256')!=patch:
   raise ValueError('The reviewed sigreturn baseline is required')
  for key,name in [('ohosGuestProtectionPatchSha256','fex-ohos-guest-protection.patch'),
                   ('ohosGuestProtectionHeaderSha256','fex-ohos-guest-protection.h')]:
   if receipt['inputs'].get(key)!=sha(ROOT/'scripts/patches'/name):
    raise ValueError('Missing or stale OHOS guest protection input: '+name)
  if receipt['inputs'].get('gameContractSha256')!=sha(ROOT/'scripts/patches/fex-valve-2607-game-contract.patch') or receipt['inputs'].get('builderSha256')!=sha(ROOT/'scripts/build_linux_native_fex.py'):
   raise ValueError('Stale FEX production inputs')
  thunks=json.loads((a.gl_thunks/'build.json').read_text())
  if thunks['inputs']['sourceSha256']!=sha(a.fex_build/'sigreturn-manifest.json') or thunks['inputs']['scriptSha256']!=sha(ROOT/'scripts/build_linux_native_gl_thunks.py'):
   raise ValueError('FEX and GL thunks must be built as one source group')
  directory.mkdir(parents=True,exist_ok=True)
  inputs={'FEX':a.fex_build/'build/Bin/FEX','FEXServer':a.fex_build/'build/Bin/FEXServer',
   'libvulkan_virtio.so':a.venus,'libsteamohos_software_gl.so':a.software_gl}
  for bits in (32,64):
   for side in ('host','guest'):
    folder=('HostThunks' if side=='host' else 'GuestThunks')+('_32' if bits==32 else '')
    name=f'libGL-{side}{bits}.so'
    source=a.gl_thunks/folder/f'libGL-{side}.so'
    matching=[row for row in thunks['files'].values() if Path(row['path']).name==source.name and Path(row['path']).parent.name==folder]
    if len(matching)!=1 or matching[0]['sha256']!=sha(source):raise ValueError('GL thunk build receipt mismatch')
    inputs[name]=source
  for name,source in inputs.items():
   if name in receipt['executables'] and sha(source)!=receipt['executables'][name]['sha256']:
    raise ValueError('FEX build receipt mismatch')
   target=directory/name
   if not target.exists() or sha(target)!=sha(source):shutil.copyfile(source,target)
  record=dict(schemaVersion=1,architecture='aarch64-linux-gnu',
   sourceRevision=receipt['source']['revision'],upstreamSigreturnPatchSha256=patch,
   upstreamStatus='published issue diff; not merged upstream',
   sourceSubmodules=receipt['source']['submodules'],buildInputs=receipt['inputs'],
   glThunkInputs=thunks['inputs'],
   sources={n:sha(ROOT/n) for n in SOURCE_NAMES},
   files={n:dict(bytes=(directory/n).stat().st_size,sha256=sha(directory/n)) for n in NAMES})
  (directory/'manifest.json').write_text(json.dumps(record,indent=2)+'\n')
 v=verify();print('Verified HAP native repairs:',sum(row['bytes'] for row in v['files'].values()),'bytes')
if __name__=='__main__':main()
