#!/usr/bin/env python3
"""Restore a paired, minimal x86 GNU ABI closure for official FEX guest thunks.

No GPU ICD, emulator, game, or complete rootfs is included. Ubuntu packages are
pinned by SHA-256; only the declared flat library members are extracted.
"""
import argparse,hashlib,json,os,subprocess,tarfile,urllib.request
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
PACKAGES=[
 ('glibc','pool/main/g/glibc/libc6_2.35-0ubuntu3.15_amd64.deb','79e35256227e16a607c154cdeb8d76ff12d20e31de286ea7fd9ad3b96fe0452d'),
 ('gcc','pool/main/g/gcc-12/libgcc-s1_12.3.0-1ubuntu1~22.04.3_amd64.deb','d383e642d83263147f7b4b69fb92cf1036e5246c6be3accf10b77f986684589a'),
 ('gcc','pool/main/g/gcc-12/libstdc++6_12.3.0-1ubuntu1~22.04.3_amd64.deb','29ef3d289b272704b75141906c8f683db0f2bdb0942061366691333bfb289bdc'),
]
NAMES=['ld-linux-x86-64.so.2','libc.so.6','libm.so.6','libdl.so.2','libpthread.so.0',
 'librt.so.1','libutil.so.1','libresolv.so.2','libanl.so.1','libnss_dns.so.2',
 'libnss_files.so.2','libgcc_s.so.1','libstdc++.so.6','libudev.so.1','libasound_module_pcm_hmosdeck.so',
 'libsteamohos_native_ipc.so']
ADAPTER_SOURCES=['entry/src/main/cpp/linux_steam/gnu_fixture/native_udev.c',
 'entry/src/main/cpp/linux_steam/gnu_fixture/native_udev_api.h',
 'entry/src/main/cpp/linux_steam/gnu_fixture/native_guest_audio.c',
 'entry/src/main/cpp/linux_steam/gnu_fixture/alsa_hmosdeck.c',
 'entry/src/main/cpp/protocols/audio_ipc_protocol.h',
 'entry/src/main/cpp/linux_steam/gnu_fixture/sysvipc_compat.c',
 'scripts/prepare_linux_native_guest_core.py']
def adapter_sources(root):return {name:sha(root/name) for name in ADAPTER_SOURCES}
def sha(p):
 h=hashlib.sha256()
 with p.open('rb') as f:
  while block:=f.read(1024*1024):h.update(block)
 return h.hexdigest()
def verify(root=ROOT,hap=None):
 out=root/'entry/src/main/resources/rawfile/linux-steam/bootstrap/native-guest-core-x86_64'
 manifest=json.loads((out/'manifest.json').read_text())
 if manifest['architecture']!='x86_64-linux-gnu' or set(manifest['files'])!=set(NAMES):raise ValueError('Guest core manifest contract')
 if manifest.get('adapterSources')!=adapter_sources(root):raise ValueError('Guest udev adapter source mismatch')
 if manifest['packages']!=[dict(source=s,path=p,sha256=h) for s,p,h in PACKAGES]:raise ValueError('Guest core package lock mismatch')
 for name,record in manifest['files'].items():
  p=out/name
  if p.is_symlink() or p.stat().st_size!=record['bytes'] or sha(p)!=record['sha256']:raise ValueError('Guest core mismatch: '+name)
  data=p.read_bytes()
  if data[:6]!=b'\x7fELF\x02\x01' or int.from_bytes(data[18:20],'little')!=62:raise ValueError('Guest core ELF: '+name)
  if hap:
   relative=p.relative_to(root/'entry/src/main/resources/rawfile').as_posix()
   payload=hap.read('resources/rawfile/'+relative)
   if len(payload)!=len(data) or hashlib.sha256(payload).hexdigest()!=record['sha256']:raise ValueError('HAP guest core: '+name)
 if hap:
  relative=(out/'manifest.json').relative_to(root/'entry/src/main/resources/rawfile').as_posix()
  if hap.read('resources/rawfile/'+relative)!=(out/'manifest.json').read_bytes():raise ValueError('HAP guest core manifest')
 return manifest
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--verify-only',action='store_true');p.add_argument('--cache',type=Path,default=ROOT/'build/native-guest-core-downloads');a=p.parse_args()
 try:
  m=verify();print('Verified/reused native x86 GNU core:',sum(x['bytes'] for x in m['files'].values()),'bytes');return
 except (OSError,ValueError,KeyError):
  if a.verify_only:raise
 out=ROOT/'entry/src/main/resources/rawfile/linux-steam/bootstrap/native-guest-core-x86_64';out.mkdir(parents=True,exist_ok=True);a.cache.mkdir(parents=True,exist_ok=True)
 proxy=out/'libudev.so.1'
 compiler=os.environ.get('STEAMOHOS_GUEST_CC','cc')
 machine=subprocess.check_output([compiler,'-dumpmachine'],text=True).strip()
 if not machine.startswith('x86_64') or 'linux' not in machine:raise ValueError('Expected x86_64 GNU guest compiler')
 pending=proxy.with_suffix('.pending')
 subprocess.run([compiler,'-O2','-g0','-Wall','-Wextra','-Werror','-fPIC','-shared',
  str(ROOT/ADAPTER_SOURCES[0]),'-ldl','-pthread','-Wl,-z,relro,-z,now',
  '-Wl,-soname,libsteamohos_native_udev.so.1','-o',str(pending)],check=True)
 pending.replace(proxy)
 audio=out/'libasound_module_pcm_hmosdeck.so';pending=audio.with_suffix('.pending')
 subprocess.run([compiler,'-O2','-g0','-Wall','-Wextra','-Werror','-fPIC','-shared',
  '-DPIC',str(ROOT/ADAPTER_SOURCES[2]),'-lasound','-Wl,-z,relro,-z,now',
  '-o',str(pending)],check=True)
 pending.replace(audio)
 ipc=out/'libsteamohos_native_ipc.so';pending=ipc.with_suffix('.pending')
 # Guest libc's inline SysV syscalls bypass the ARM64 interposer. Reuse the
 # same app-private file/lock protocol as Steam, compiled for the guest ABI.
 subprocess.run([compiler,'-O2','-g0','-Wall','-Wextra','-Werror','-fPIC','-shared',
  str(ROOT/'entry/src/main/cpp/linux_steam/gnu_fixture/sysvipc_compat.c'),
  '-pthread','-Wl,-z,relro,-z,now','-o',str(pending)],check=True)
 pending.replace(ipc)
 chosen={'libudev.so.1':proxy.read_bytes(),audio.name:audio.read_bytes(),ipc.name:ipc.read_bytes()};links={};copyrights={}  
 for source,relative,digest in PACKAGES:
  package=a.cache/Path(relative).name
  pending=package.with_suffix('.part')
  if pending.is_file() and sha(pending)==digest:pending.replace(package)
  if not package.is_file() or sha(package)!=digest:
   pending=package.with_suffix('.part');total=0
   with urllib.request.urlopen('https://archive.ubuntu.com/ubuntu/'+relative,timeout=45) as response,pending.open('wb') as f:
    while block:=response.read(1024*1024):
     total+=len(block)
     if total>16*1024*1024:raise ValueError('Guest core package size limit')
     f.write(block)
   if sha(pending)!=digest:raise ValueError('Guest core package checksum')
   pending.replace(package)
  proc=subprocess.Popen(['dpkg-deb','--fsys-tarfile',str(package)],stdout=subprocess.PIPE)
  with tarfile.open(fileobj=proc.stdout,mode='r|') as archive:
   for member in archive:
    name=member.name.removeprefix('./');leaf=Path(name).name
    if name.endswith('/copyright') and source not in copyrights and member.isfile() and member.size<262144:
     copyrights[source]=archive.extractfile(member).read()
    if not (name.startswith('lib/x86_64-linux-gnu/') or name.startswith('usr/lib/x86_64-linux-gnu/')):continue
    if leaf in NAMES and member.issym():links[leaf]=Path(member.linkname).name
    if member.isfile() and (leaf in NAMES or leaf.startswith(('libstdc++.so.6.','libgcc_s.so.1.'))) and member.size<=8*1024*1024:
     chosen[leaf]=archive.extractfile(member).read()
  proc.stdout.close()
  if proc.wait(timeout=15):raise ValueError('Guest core package decompression failed')
 records={}
 for name in NAMES:
  data=chosen[links.get(name,name)]
  if data[:6]!=b'\x7fELF\x02\x01' or int.from_bytes(data[18:20],'little')!=62:raise ValueError('Guest core ELF member')
  dest=out/name
  if not dest.is_file() or dest.read_bytes()!=data:
   pending=dest.with_suffix('.pending');pending.write_bytes(data);pending.replace(dest)
  records[name]={'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest()}
 for source,data in copyrights.items():(out/(source+'-copyright.txt')).write_bytes(data)
 manifest=dict(schemaVersion=1,architecture='x86_64-linux-gnu',packages=[dict(source=s,path=p,sha256=h) for s,p,h in PACKAGES],adapterSources=adapter_sources(ROOT),files=records)
 (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
 verify();print('Prepared native x86 GNU core:',sum(x['bytes'] for x in records.values()),'bytes')
if __name__=='__main__':main()
