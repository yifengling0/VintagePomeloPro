#!/usr/bin/env python3
"""Verify the production cleanup retains Mesa EGL and removes SDK linker stubs."""
import argparse
import hashlib
from pathlib import Path
import shutil
import subprocess

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--root',type=Path,required=True)
p.add_argument('--sdk',type=Path,required=True)
p.add_argument('--mesa-egl',type=Path,required=True)
p.add_argument('--build-dir',type=Path,required=True)
a=p.parse_args()
s=(a.root/'scripts/build_native.sh').read_text()
start=s.index('remove_native_egl_linker_stubs() {')
function=s[start:s.index('\n}',start)+2]
stub=a.sdk/'native/sysroot/usr/lib/aarch64-linux-ohos/libEGL.so'
assert stub.is_file() and a.mesa_egl.is_file()
for label,source,keep in [('mesa',a.mesa_egl,True),('sdk',stub,False)]:
    directory=(a.build_dir/label).resolve();directory.mkdir(parents=True,exist_ok=True)
    for name in ('libEGL.so','libEGL.so.1'):shutil.copyfile(source,directory/name)
    script='set -euo pipefail\nlog(){ :; }\nerr(){ exit 1; }\n'+function+'\nremove_native_egl_linker_stubs\n'
    import os
    subprocess.run(['bash'],input=script,text=True,check=True,
        env=dict(os.environ,OHOS_SDK=str(a.sdk),NATIVE_LIBS=str(directory)))
    for name in ('libEGL.so','libEGL.so.1'):
        target=directory/name
        assert target.exists()==keep,(label,name)
        if keep:assert hashlib.sha256(target.read_bytes()).digest()==hashlib.sha256(source.read_bytes()).digest()
print('PASS production EGL cleanup: both Mesa aliases preserved byte-for-byte; both SDK stubs removed')
