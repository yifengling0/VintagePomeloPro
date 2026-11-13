#!/usr/bin/env python3
"""Build isolated GNU Venus presentation tests and Wine WSI adapter from existing dependencies."""
from pathlib import Path
import argparse,hashlib,json,subprocess

def sha(p):
 h=hashlib.sha256()
 with p.open('rb') as f:
  for block in iter(lambda:f.read(1024*1024),b''):h.update(block)
 return h.hexdigest()

p=argparse.ArgumentParser(description=__doc__)
for name in ['cc','mesa-build','wayland-build','runtime','output']:p.add_argument('--'+name,type=Path,required=True)
a=p.parse_args();repo=Path(__file__).resolve().parents[1]
cc=a.cc.resolve(strict=True);cxx=cc.with_name(cc.name.replace('gcc','g++'))
mesa=a.mesa_build.resolve(strict=True);wayland=a.wayland_build.resolve(strict=True);runtime=a.runtime.resolve(strict=True)
out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
if out.is_relative_to(repo) or out.is_relative_to(mesa):p.error('Use the existing external experiment directory')
assert subprocess.check_output([str(cc),'-dumpmachine'],text=True).strip().startswith('aarch64')
scanner=wayland/'build/native-scanner/bin/wayland-scanner'
xml=mesa/'deps-root/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml'
fixtures=repo/'entry/src/main/cpp/linux_steam/gnu_fixture'
headers=[mesa/'mesa-source/include/vulkan/vulkan_core.h',wayland/'build/gnu-deps/include/wayland-client.h']
inputs={str(x):sha(x) for x in [Path(__file__),cc,cxx,scanner,xml,*headers,runtime/'libwayland-client.so.0',fixtures/'proton_vulkan.c',fixtures/'proton_vulkan_wsi.cpp']}
record=out/'proton-vulkan-build.json'
if record.exists():
 old=json.loads(record.read_text())
 if old.get('inputs')==inputs and all((out/k).exists() and sha(out/k)==v['sha256'] for k,v in old.get('outputs',{}).items()) and len(old.get('outputs',{}))==2:
  print('Reused GNU Vulkan fixtures and adapter');raise SystemExit(0)
header=out/'xdg-shell-client-protocol.h';code=out/'xdg-shell-protocol.c'
subprocess.run([str(scanner),'client-header',str(xml),str(header)],check=True)
subprocess.run([str(scanner),'private-code',str(xml),str(code)],check=True)
common=['-O2','-Wall','-Wextra','-Werror','-I'+str(out),'-I'+str(mesa/'mesa-source/include'),'-I'+str(wayland/'build/gnu-deps/include')]
libs=['-L'+str(runtime),'-Wl,-rpath-link,'+str(runtime),'-l:libwayland-client.so.0','-ldl','-pthread']
subprocess.run([str(cc),*common,str(fixtures/'proton_vulkan.c'),str(code),*libs,'-o',str(out/'proton-vulkan')],check=True)
subprocess.run([str(cxx),*common,'-Wno-misleading-indentation','-shared','-fPIC',str(fixtures/'proton_vulkan_wsi.cpp'),*libs,'-o',str(out/'libproton_vulkan.so')],check=True)
record.write_text(json.dumps({'inputs':inputs,'outputs':{name:{'bytes':(out/name).stat().st_size,'sha256':sha(out/name)} for name in ['proton-vulkan','libproton_vulkan.so']},'scope':'private GNU experiment; requires separately verified Venus ICD'},indent=2)+'\n')
print(record.read_text())
