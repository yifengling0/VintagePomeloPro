#!/usr/bin/env python3
"""Build and run the real native BC command recorder with stub Vulkan calls."""
import argparse, subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--build-dir',type=Path,required=True)
p.add_argument('--shader-dir',type=Path)
p.add_argument('--glslang',type=Path,default=Path('/usr/bin/glslangValidator'))
p.add_argument('--sanitize',action='store_true')
a=p.parse_args();out=a.build_dir.resolve();out.mkdir(parents=True,exist_ok=True)
shaders=a.shader_dir.resolve() if a.shader_dir else out
subprocess.run(['python3',str(ROOT/'scripts/diagnostics/build_native_bc_shaders.py'),
    '--output-dir',str(shaders),'--glslang',str(a.glslang)],check=True,timeout=120)
scanner='wayland-scanner'
xml=Path('/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml')
if not xml.exists():xml=Path('/home/liufeng/steamohos-software-gl-20261019-v6/deps-root/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml')
for kind,name in [('client-header','xdg-shell-client-protocol.h'),('private-code','xdg-shell-protocol.c')]:
    subprocess.run([scanner,kind,str(xml),str(out/name)],check=True,timeout=15)
obj=out/'protocol-host.o'
subprocess.run(['cc','-fPIC','-c',str(out/'xdg-shell-protocol.c'),'-o',str(obj)],check=True,timeout=30)
binary=out/('recording-sanitized' if a.sanitize else 'recording-test')
flags=['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'] if a.sanitize else ['-O2']
subprocess.run(['g++','-std=c++17',*flags,'-Wall','-Wextra','-Werror',
    '-Wno-misleading-indentation','-Wno-missing-field-initializers',
    '-I'+str(out),'-I'+str(shaders),'-I'+str(ROOT/'thirdparty/mesa/include'),
    str(ROOT/'host_tests/native_bc_recording_test.cpp'),str(obj),
    '-lwayland-client','-lxcb','-ldl','-pthread','-o',str(binary)],check=True,timeout=120)
subprocess.run([str(binary)],check=True,timeout=15)
