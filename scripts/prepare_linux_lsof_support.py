#!/usr/bin/env python3
"""Build/verify the HAP-owned registry-aware lsof without repacking GNU ZIPs."""
import argparse,hashlib,json,shutil,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
RAW='entry/src/main/resources/rawfile/linux-steam/bootstrap'
SOURCES=['scripts/prepare_linux_lsof_support.py','scripts/build_linux_steam_lsof.py','scripts/linux_component_identity.py',
 'patches/lsof/0001-ohos-live-socket-query-backend.patch','patches/lsof/0002-cooperative-tcp-registry-scan.patch',
 'entry/src/main/cpp/linux_steam/gnu_fixture/lsof_socket_backend.h','entry/src/main/cpp/linux_steam/gnu_fixture/socket_query_protocol.h']
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def verify(root=ROOT):
 directory=root/RAW;v=json.loads((directory/'lsof-manifest.json').read_text());p=directory/'steamohos_lsof_hap'
 if v.get('schemaVersion')!=1 or v.get('name')!=p.name or v.get('architecture')!='aarch64-linux-gnu' or set(v.get('sources',{}))!=set(SOURCES):raise ValueError('Invalid lsof companion inventory')
 for name,digest in v['sources'].items():
  if sha(root/name)!=digest:raise ValueError('Stale lsof companion source: '+name)
 if p.is_symlink() or p.stat().st_size!=v['bytes'] or sha(p)!=v['sha256']:raise ValueError('Invalid lsof companion payload')
 data=p.read_bytes()[:20]
 if data[:6]!=b'\x7fELF\x02\x01' or data[18:20]!=b'\xb7\x00':raise ValueError('Invalid lsof companion ABI')
 return v
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--verify-only',action='store_true');p.add_argument('--archive',type=Path);p.add_argument('--cc',default='/usr/bin/aarch64-linux-gnu-gcc');a=p.parse_args()
 if not a.verify_only:
  if not a.archive:p.error('--archive is required to rebuild')
  out=ROOT/'workspace_temp/hsl-sync/lsof';subprocess.run([sys.executable,str(ROOT/'scripts/build_linux_steam_lsof.py'),'--archive',str(a.archive),'--cc',a.cc,'--output',str(out)],check=True)
  receipt=json.loads((out/'manifest.json').read_text());target=ROOT/RAW/'steamohos_lsof_hap';source=out/'steamohos_lsof'
  if receipt['files']['steamohos_lsof']!={'bytes':source.stat().st_size,'sha256':sha(source)}:raise ValueError('Stale lsof build receipt')
  if not target.exists() or sha(target)!=sha(source):shutil.copyfile(source,target)
  v=dict(schemaVersion=1,name=target.name,architecture='aarch64-linux-gnu',bytes=target.stat().st_size,sha256=sha(target),
   sources={n:sha(ROOT/n) for n in SOURCES},buildInputs=receipt['buildInputs'],sourceUrl=receipt['sourceUrl'])
  (ROOT/RAW/'lsof-manifest.json').write_text(json.dumps(v,indent=2)+'\n')
 v=verify();print('Verified registry-aware GNU lsof:',v['bytes'],'bytes')
if __name__=='__main__':main()
