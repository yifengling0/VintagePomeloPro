#!/usr/bin/env python3
"""Build the private GNU Wayland/NativeImage transport probe from existing inputs."""
from pathlib import Path
import argparse, hashlib, json, subprocess

def sha(path):
    h=hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda:f.read(1024*1024),b''):h.update(block)
    return h.hexdigest()

p=argparse.ArgumentParser(description=__doc__)
for name in ['cc','mesa-build','wayland-build','runtime','output']:
    p.add_argument('--'+name,type=Path,required=True)
a=p.parse_args()
repo=Path(__file__).resolve().parents[1]
cc=a.cc.resolve(strict=True);mesa=a.mesa_build.resolve(strict=True)
wayland=a.wayland_build.resolve(strict=True);runtime=a.runtime.resolve(strict=True)
out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
assert not out.is_relative_to(repo) and not out.is_relative_to(mesa)
assert subprocess.check_output([str(cc),'-dumpmachine'],text=True).strip().startswith('aarch64')
scanner=wayland/'build/native-scanner/bin/wayland-scanner'
xml=mesa/'deps-root/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml'
source=repo/'entry/src/main/cpp/linux_steam/gnu_fixture/proton_present.c'
header=out/'xdg-shell-client-protocol.h';code=out/'xdg-shell-protocol.c'
inputs={str(path):sha(path) for path in [Path(__file__),source,cc,scanner,xml,
    runtime/'libwayland-client.so.0',wayland/'build/gnu-deps/include/wayland-client.h',
    mesa/'mesa-source/include/EGL/egl.h',mesa/'mesa-source/include/EGL/eglext.h']}
target=out/'proton-shared-present';record=out/'shared-present-build.json'
if record.exists() and target.exists():
    old=json.loads(record.read_text())
    if old.get('inputs')==inputs and old.get('sha256')==sha(target):
        print('Reused GNU shared-present probe');raise SystemExit(0)
subprocess.run([str(scanner),'client-header',str(xml),str(header)],check=True)
subprocess.run([str(scanner),'private-code',str(xml),str(code)],check=True)
subprocess.run([str(cc),'-O2','-Wall','-Wextra','-Werror','-I'+str(out),
    '-I'+str(mesa/'mesa-source/include'),'-I'+str(wayland/'build/gnu-deps/include'),
    str(source),str(code),'-L'+str(runtime),'-Wl,-rpath-link,'+str(runtime),
    '-l:libwayland-client.so.0','-ldl','-o',str(target)],check=True)
record.write_text(json.dumps({'inputs':inputs,'bytes':target.stat().st_size,'sha256':sha(target),
    'scope':'GNU transport only; no Wine or Steam game claim'},indent=2)+'\n')
print('GNU shared-present',target.stat().st_size,sha(target))
