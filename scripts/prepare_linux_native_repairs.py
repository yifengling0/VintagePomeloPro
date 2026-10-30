#!/usr/bin/env python3
"""Package reviewed native-game repairs beside immutable Steam/Proton archives.

Reuses explicit existing GNU build outputs. Never edits games or Valve depots.
The FEX changes are the exact published upstream diff, whose provenance ships.
"""
import argparse, hashlib, json, re, shutil
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
RELATIVE='linux-steam/bootstrap/native-game-support'
NAMES=['FEX','FEXServer','libvulkan_virtio.so','libsteamohos_software_gl.so']
SOURCE_NAMES=['scripts/prepare_linux_native_repairs.py','scripts/diagnostics/build_native_fex_pop.py',
 'scripts/patches/fex-upstream-5901-restore-sigreturn-state.patch',
 'scripts/patches/fex-upstream-5901-restore-sigreturn-state.provenance.json',
 'scripts/build_linux_proton_venus.py','scripts/build_linux_steam_software_gl.py',
 'scripts/build_linux_steam_client.py','patches/mesa/0011-softpipe-demand-zero-texture-cache.patch',
 'patches/virglrenderer/0007-ohos-native-memory-context.patch',
 'patches/virglrenderer/0008-ohos-native-resource-contract.patch',
 'entry/src/main/cpp/linux_steam/gnu_fixture/native_fex_core.h',
 'entry/src/main/resources/rawfile/'+RELATIVE+'/NOTICE.txt']
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
  if data[:6]!=b'\x7fELF\x02\x01' or data[18:20]!=b'\xb7\x00':raise ValueError('Native repair ELF mismatch')
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
 for name in ['FEX','FEXServer']:
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
 for n in ['fex-build','venus','software-gl']:p.add_argument('--'+n,type=Path)
 a=p.parse_args();directory=ROOT/'entry/src/main/resources/rawfile'/RELATIVE
 if not a.verify_only:
  if not all([a.fex_build,a.venus,a.software_gl]):p.error('Explicit previously built inputs required')
  receipt=json.loads((a.fex_build/'sigreturn-manifest.json').read_text())
  patch=sha(ROOT/SOURCE_NAMES[2])
  if receipt.get('patchSha256') or receipt.get('upstreamSigreturnPatchSha256')!=patch:
   raise ValueError('Only the reviewed upstream sigreturn fix is approved')
  directory.mkdir(parents=True,exist_ok=True)
  inputs={'FEX':a.fex_build/'build/Bin/FEX','FEXServer':a.fex_build/'build/Bin/FEXServer',
   'libvulkan_virtio.so':a.venus,'libsteamohos_software_gl.so':a.software_gl}
  for name,source in inputs.items():
   if name in receipt['executables'] and sha(source)!=receipt['executables'][name]['sha256']:
    raise ValueError('FEX build receipt mismatch')
   target=directory/name
   if not target.exists() or sha(target)!=sha(source):shutil.copyfile(source,target)
  record=dict(schemaVersion=1,architecture='aarch64-linux-gnu',
   sourceRevision=receipt['source']['revision'],upstreamSigreturnPatchSha256=patch,
   upstreamStatus='published issue diff; not merged upstream',
   sourceSubmodules=receipt['source']['submodules'],buildInputs=receipt['inputs'],
   sources={n:sha(ROOT/n) for n in SOURCE_NAMES},
   files={n:dict(bytes=(directory/n).stat().st_size,sha256=sha(directory/n)) for n in NAMES})
  (directory/'manifest.json').write_text(json.dumps(record,indent=2)+'\n')
 v=verify();print('Verified HAP native repairs:',sum(row['bytes'] for row in v['files'].values()),'bytes')
if __name__=='__main__':main()
