#!/usr/bin/env python3
"""Build/reuse the HAP-owned GNU POSIX adapter without repacking base runtimes."""
import argparse, hashlib, json, subprocess
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
NATIVE=ROOT/'entry/src/main/cpp/linux_steam'
SOURCES=[NATIVE/'gnu_fixture'/name for name in [
    'posix_compat.c','sysvipc_compat.c','filesystem_compat.c','popen_compat.c',
    'socket_metadata.c','exec_compat.cpp','syscall_compat.S']]+[NATIVE/'native_loader.cpp',NATIVE/'native_loader_entry.S']

def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cc',default='aarch64-linux-gnu-gcc')
    p.add_argument('--verify-only',action='store_true')
    p.add_argument('--rebuild',action='store_true')
    p.add_argument('--build-dir',type=Path,default=ROOT/'workspace_temp/linux-proton/posix-hap-build')
    a=p.parse_args()
    wrapper=NATIVE/'gnu_fixture/steamohos_fex.cpp'
    inputs=[*SOURCES,wrapper,*sorted(NATIVE.glob('*.h')),*sorted((NATIVE/'gnu_fixture').glob('*.h')),Path(__file__).resolve()]
    sources={str(path.relative_to(ROOT)):sha(path) for path in inputs}
    output=ROOT/'entry/src/main/resources/rawfile/linux-steam/bootstrap'
    binary=output/'libsteamohos_posix_hap.so';manifest=output/'posix-manifest.json'
    old=json.loads(manifest.read_text()) if manifest.exists() else {}
    fex=output/'steamohos_fex';fex_manifest=output/'fex-manifest.json'
    prior=json.loads(fex_manifest.read_text()) if fex_manifest.exists() else {}
    wrapper_valid=fex.is_file() and prior.get('sources')==sources and prior.get('sha256')==sha(fex)
    if not a.rebuild and wrapper_valid and old.get('sources')==sources and binary.is_file() and old.get('sha256')==sha(binary):
        print('Verified/reused HAP GNU POSIX adapter:',binary.stat().st_size,'bytes');return
    if a.verify_only:raise ValueError('HAP POSIX source/payload changed; rebuild with a GNU ARM64 compiler')
    machine=subprocess.check_output([a.cc,'-dumpmachine'],text=True).strip()
    if not machine.startswith('aarch64') or 'linux-gnu' not in machine:raise ValueError('Expected GNU ARM64 compiler')
    cxx=a.cc[:-3]+'g++' if a.cc.endswith('gcc') else 'aarch64-linux-gnu-g++'
    a.build_dir.mkdir(parents=True,exist_ok=True);output.mkdir(parents=True,exist_ok=True)
    common=['-O2','-g0','-Wall','-Wextra','-fPIC','-Wl,-z,relro,-z,now']
    objects=[]
    for source in SOURCES:
        target=a.build_dir/(source.name+'.o');objects.append(target)
        compiler=cxx if source.suffix=='.cpp' else a.cc
        flags=['-std=c++17'] if source.suffix=='.cpp' else []
        subprocess.run([compiler,*common,*flags,'-c',str(source),'-o',str(target)],check=True)
    pending=binary.with_suffix('.pending')
    subprocess.run([cxx,*common,'-shared',*map(str,objects),'-pthread','-ldl',
        '-Wl,-soname,libsteamohos_posix.so','-o',str(pending)],check=True)
    data=pending.read_bytes()
    if data[:6]!=b'\x7fELF\x02\x01' or int.from_bytes(data[18:20],'little')!=183:raise ValueError('Invalid ARM64 ELF')
    pending.replace(binary)
    manifest.write_text(json.dumps(dict(schemaVersion=1,architecture='aarch64-linux-gnu',name=binary.name,
        bytes=len(data),sha256=sha(binary),sources=sources),indent=2)+'\n')
    pending=fex.with_suffix('.pending')
    subprocess.run([cxx,*common,'-std=c++17',str(wrapper),'-o',str(pending)],check=True)
    pending.replace(fex)
    fex_manifest.write_text(json.dumps(dict(schemaVersion=1,architecture='aarch64-linux-gnu',name=fex.name,
        bytes=fex.stat().st_size,sha256=sha(fex),sources=sources),indent=2)+'\n')
    print('Built HAP GNU POSIX adapter:',len(data),'bytes; native FEX entry:',fex.stat().st_size,'bytes')

if __name__=='__main__':main()
