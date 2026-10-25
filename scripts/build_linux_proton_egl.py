#!/usr/bin/env python3
"""Build the private GNU Wine Wayland EGL shared-present adapter."""
from pathlib import Path
import argparse,hashlib,json,subprocess

def sha(path):
    h=hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda:f.read(1024*1024),b''):h.update(block)
    return h.hexdigest()
p=argparse.ArgumentParser(description=__doc__)
for name in ['cxx','mesa-build','wayland-build','wayland-source','runtime','output']:
    p.add_argument('--'+name,type=Path,required=True)
a=p.parse_args();repo=Path(__file__).resolve().parents[1]
source=repo/'entry/src/main/cpp/linux_steam/gnu_fixture/proton_egl.cpp'
out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
assert not out.is_relative_to(repo)
cxx=a.cxx.resolve(strict=True)
assert subprocess.check_output([str(cxx),'-dumpmachine'],text=True).strip().startswith('aarch64')
inputs={str(path):sha(path) for path in [Path(__file__),source,cxx,
    a.wayland_source/'egl/wayland-egl-backend.h',a.runtime/'libwayland-client.so.0',
    a.output/'libEGL-proton.so.1']}
target=out/'libproton_egl.so';record=out/'proton-egl-build.json'
if record.exists() and target.exists():
    old=json.loads(record.read_text())
    if old.get('inputs')==inputs and old.get('sha256')==sha(target):
        print('Reused GNU Wine EGL adapter');raise SystemExit(0)
subprocess.run([str(cxx),'-std=c++17','-O2','-fPIC','-shared','-Wall','-Wextra','-Werror',
    '-I'+str(a.mesa_build/'mesa-source/include'),'-I'+str(a.wayland_build/'build/gnu-deps/include'),
    '-I'+str(a.wayland_source/'egl'),str(source),'-L'+str(a.runtime),
    '-Wl,-rpath-link,'+str(a.runtime),'-Wl,-soname,libproton_egl.so',
    '-l:libwayland-client.so.0','-ldl','-pthread','-o',str(target)],check=True)
record.write_text(json.dumps({'inputs':inputs,'bytes':target.stat().st_size,'sha256':sha(target),
    'scope':'Opt-in GNU Wine EGL experiment; no product runtime replacement'},indent=2)+'\n')
print('GNU Wine EGL adapter',target.stat().st_size,sha(target))
