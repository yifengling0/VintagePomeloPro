#!/usr/bin/env python3
"""Build the HAP-owned X11 session host without changing Steam archives."""
import argparse, hashlib, json, subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
FIXTURE=ROOT/'entry/src/main/cpp/linux_steam/gnu_fixture'
SOURCES=[FIXTURE/n for n in ['desktop_session.c','wine_window_bridge.h',
 'deck_session_control.h','deck_action_uri.h','pointer_observer.h']]+[Path(__file__).resolve()]
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument('--cc',default='aarch64-linux-gnu-gcc')
 p.add_argument('--x11-deps',type=Path,help='Existing GNU ARM64 dependency prefix (include/ and lib/aarch64-linux-gnu/)')
 p.add_argument('--verify-only',action='store_true')
 a=p.parse_args();raw=ROOT/'entry/src/main/resources/rawfile/linux-steam/bootstrap'
 binary=raw/'steamohos_gnu_desktop_hap';manifest=raw/'desktop-manifest.json'
 sources={str(s.relative_to(ROOT)):sha(s) for s in SOURCES}
 old=json.loads(manifest.read_text()) if manifest.is_file() else {}
 if old.get('sources')==sources and binary.is_file() and old.get('sha256')==sha(binary):
  print('Verified/reused HAP X11 session host:',binary.stat().st_size,'bytes');return
 if a.verify_only:raise ValueError('X11 session host source/payload changed; rebuild GNU ARM64 host')
 target=subprocess.check_output([a.cc,'-dumpmachine'],text=True).strip()
 if not target.startswith('aarch64') or 'linux-gnu' not in target:raise ValueError('Expected GNU ARM64 compiler')
 flags=[]
 if a.x11_deps:
  deps=a.x11_deps.resolve(strict=True);lib=deps/'lib/aarch64-linux-gnu'
  flags=['-I'+str(deps/'include'),'-L'+str(lib),'-Wl,-rpath-link,'+str(lib)]
 raw.mkdir(parents=True,exist_ok=True);pending=binary.with_suffix('.pending')
 subprocess.run([a.cc,'-O2','-g0','-Wall','-Wextra','-Werror','-fPIE','-pie',
  '-Wl,-z,relro,-z,now',*flags,str(FIXTURE/'desktop_session.c'),'-lX11','-ldl','-o',str(pending)],check=True)
 data=pending.read_bytes()
 if data[:6]!=b'\x7fELF\x02\x01' or data[18:20]!=b'\xb7\x00':raise ValueError('Invalid GNU ARM64 host ELF')
 if not 1024<=len(data)<=262144:raise ValueError('X11 session host exceeds delivery limit')
 pending.replace(binary)
 manifest.write_text(json.dumps(dict(schemaVersion=1,architecture='aarch64-linux-gnu',
  name=binary.name,bytes=len(data),sha256=sha(binary),sources=sources),indent=2)+'\n')
 print('Built HAP X11 session host:',len(data),'bytes')
if __name__=='__main__':main()
