#!/usr/bin/env python3
"""Build an isolated GNU adapter with allocator observations; no release writes."""
import argparse,hashlib,json,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
NATIVE=ROOT/'entry/src/main/cpp/linux_steam'
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument('--output-dir',required=True,type=Path)
 p.add_argument('--cc',default='aarch64-linux-gnu-gcc')
 p.add_argument('--fault-origin',action='store_true',help='Observe bounded original host MAPERR contexts before FEX forwards them')
 a=p.parse_args();out=a.output_dir.resolve();out.mkdir(parents=True,exist_ok=True)
 sources=[NATIVE/'gnu_fixture'/n for n in ['posix_compat.c','sysvipc_compat.c','filesystem_compat.c','popen_compat.c','socket_metadata.c','exec_compat.cpp','syscall_compat.S']]+[NATIVE/'native_loader.cpp',NATIVE/'native_loader_entry.S']
 diagnostic=Path(__file__).with_name('native_heap_observer.h')
 inputs={str(f.relative_to(ROOT)):sha(f) for f in [*sources,*NATIVE.glob('*.h'),*(NATIVE/'gnu_fixture').glob('*.h'),diagnostic,Path(__file__)]}
 inputs['faultOriginEnabled']=a.fault_origin
 record=out/'native-heap-observer.json';binary=out/'libsteamohos_posix_hap.so'
 prior=json.loads(record.read_text()) if record.exists() else {}
 if prior.get('inputs')==inputs and binary.exists() and prior.get('sha256')==sha(binary):
  print('Reused heap observer');return
 machine=subprocess.check_output([a.cc,'-dumpmachine'],text=True).strip()
 assert machine.startswith('aarch64') and 'linux-gnu' in machine
 cxx=a.cc[:-3]+'g++'
 wrapper=out/'posix_heap.c';wrapper.write_text('#include "'+str(sources[0])+'"\n#include "'+str(diagnostic)+'"\n')
 objects=[]
 for source in sources:
  target=out/(source.name+'.o');objects.append(target)
  actual=wrapper if source.name=='posix_compat.c' else source
  compiler=cxx if source.suffix=='.cpp' else a.cc
  subprocess.run([compiler,'-O2','-g0','-fPIC','-Wall','-Wextra','-Werror',*(['-DSTEAMOHOS_NATIVE_FAULT_ORIGIN=1'] if a.fault_origin else []),*(['-std=c++17'] if source.suffix=='.cpp' else []),'-c',str(actual),'-o',str(target)],check=True,timeout=60)
 subprocess.run([cxx,'-shared',*map(str,objects),'-pthread','-ldl','-Wl,-z,relro,-z,now','-Wl,-soname,libsteamohos_posix.so','-o',str(binary)],check=True,timeout=60)
 record.write_text(json.dumps(dict(inputs=inputs,sha256=sha(binary),bytes=binary.stat().st_size,experimental=True,releasePayload=False),indent=2)+'\n')
 print('Built heap observer',sha(binary))
if __name__=='__main__':main()
