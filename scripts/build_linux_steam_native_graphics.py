#!/usr/bin/env python3
"""Build the HAP-owned native Linux Vulkan WSI over the existing GNU Venus ICD.

Reuse Mesa/Wayland headers and native scanner; never rebuild or copy full Mesa.
The separate binary does not replace the verified Wine Vulkan WSI component.
"""
import argparse, hashlib, json, subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
FIXTURE=ROOT/'entry/src/main/cpp/linux_steam/gnu_fixture'
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument('--verify-only',action='store_true')
 p.add_argument('--glslang',type=Path,default=Path('/usr/bin/glslangValidator'))
 for name in ['cc','mesa-build','wayland-build','runtime','build-dir']:p.add_argument('--'+name,type=Path)
 a=p.parse_args();sources={str(x.relative_to(ROOT)):sha(x) for x in [Path(__file__).resolve(),
  FIXTURE/'native_vulkan_wsi.cpp',FIXTURE/'native_x11_window.h',FIXTURE/'proton_vulkan_wsi.cpp',
  ROOT/'scripts/diagnostics/build_native_bc_shaders.py',*sorted((FIXTURE/'native_bc').glob('*')),
  ROOT/'thirdparty/mesa/src/vulkan/registry/vk.xml',*sorted((ROOT/'thirdparty/mesa/include/vulkan').glob('*.h'))] if x.is_file()}
 raw=ROOT/'entry/src/main/resources/rawfile/linux-steam/bootstrap'
 binary=raw/'libsteamohos_native_vulkan.so';manifest=raw/'native-graphics-manifest.json'
 prior=json.loads(manifest.read_text()) if manifest.exists() else {}
 if prior.get('sources')==sources and binary.is_file() and prior.get('sha256')==sha(binary):
  print('Verified/reused native Linux Vulkan WSI:',binary.stat().st_size,'bytes');return
 if a.verify_only:raise ValueError('Native Vulkan source/payload changed; rebuild companion')
 if not all([a.cc,a.mesa_build,a.wayland_build,a.runtime,a.build_dir]):p.error('Build requires GNU compiler and existing dependency trees')
 cc=a.cc.resolve(strict=True);cxx=cc.with_name(cc.name.replace('gcc','g++'))
 if not subprocess.check_output([str(cc),'-dumpmachine'],text=True).startswith('aarch64'):raise ValueError('Expected GNU ARM64 compiler')
 out=a.build_dir;out.mkdir(parents=True,exist_ok=True)
 subprocess.run(['python3',str(ROOT/'scripts/diagnostics/build_native_bc_shaders.py'),
  '--output-dir',str(out),'--glslang',str(a.glslang.resolve(strict=True))],check=True,timeout=120)
 mesa=a.mesa_build.resolve(strict=True);wayland=a.wayland_build.resolve(strict=True);runtime=a.runtime.resolve(strict=True)
 scanner=wayland/'build/native-scanner/bin/wayland-scanner'
 xml=mesa/'deps-root/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml'
 header=out/'xdg-shell-client-protocol.h';protocol=out/'xdg-shell-protocol.c';obj=out/'xdg-shell-protocol.o'
 subprocess.run([str(scanner),'client-header',str(xml),str(header)],check=True)
 subprocess.run([str(scanner),'private-code',str(xml),str(protocol)],check=True)
 flags=['-O2','-g0','-fPIC','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
  '-Wno-missing-field-initializers','-I'+str(out),'-I'+str(ROOT/'thirdparty/mesa/include'),'-I'+str(mesa/'deps-root/usr/include'),
  '-I'+str(wayland/'build/gnu-deps/include')]
 subprocess.run([str(cc),*flags,'-c',str(protocol),'-o',str(obj)],check=True)
 pending=binary.with_suffix('.pending')
 subprocess.run([str(cxx),*flags,'-std=c++17','-shared',str(FIXTURE/'native_vulkan_wsi.cpp'),str(obj),
  '-L'+str(runtime),'-L'+str(mesa/'deps-root/usr/lib/aarch64-linux-gnu'),
  '-Wl,-rpath-link,'+str(runtime)+':'+str(mesa/'deps-root/usr/lib/aarch64-linux-gnu'),'-Wl,-z,relro,-z,now',
  '-l:libwayland-client.so.0','-l:libxcb.so.1','-ldl','-pthread','-o',str(pending)],check=True)
 data=pending.read_bytes()
 if data[:6]!=b'\x7fELF\x02\x01' or int.from_bytes(data[18:20],'little')!=183:raise ValueError('Invalid GNU ARM64 WSI')
 pending.replace(binary)
 needed=subprocess.check_output(['readelf','-d',str(binary)],text=True)
 manifest.write_text(json.dumps(dict(schemaVersion=1,architecture='aarch64-linux-gnu',name=binary.name,
  bytes=len(data),sha256=sha(binary),sources=sources,transport='venus-nativeimage',
  x11Window='dedicated Wayland surface with XTEST input; no pixel readback',
  bcAdapter='capability-gated GPU BC to ETC2/EAC; retained original blocks',
  generated={n:sha(out/n) for n in ['native_bc_spirv.h','native_bc_sizes.h']},
  toolchain={str(x):sha(x) for x in [cc,cxx,scanner,xml]},needed=needed),indent=2)+'\n')
 print('Built native Linux Vulkan WSI:',len(data),'bytes')
if __name__=='__main__':main()
