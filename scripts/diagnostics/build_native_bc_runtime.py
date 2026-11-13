#!/usr/bin/env python3
"""Build the isolated BC WSI companion. Never replaces a release payload."""
import argparse, hashlib, json, subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output-dir', type=Path, required=True)
    p.add_argument('--protocol-dir', type=Path, required=True)
    p.add_argument('--glslang', type=Path, required=True)
    a=p.parse_args(); out=a.output_dir.resolve(); out.mkdir(parents=True, exist_ok=True)
    subprocess.run(['python3', str(Path(__file__).with_name('build_native_bc_shaders.py')),
        '--output-dir', str(out), '--glslang', str(a.glslang.resolve())], check=True, timeout=120)
    fixture=ROOT/'entry/src/main/cpp/linux_steam/gnu_fixture'
    source=fixture/'proton_vulkan_wsi.cpp'
    text=source.read_text()
    old='%s/native-game-support/libvulkan_virtio.so'
    assert text.count(old)==1
    text=text.replace(old,'%s/native-vulkan-driver/libvulkan_virtio-native-diagnostic.so')
    translation=out/'native-bc-wsi.cpp'
    translation.write_text('#define STEAMOHOS_NATIVE_FEX_WSI 1\n#define STEAMOHOS_NATIVE_BC 1\n'+text)
    manifest=json.loads((ROOT/'entry/src/main/resources/rawfile/linux-steam/bootstrap/native-graphics-manifest.json').read_text())
    compiler=next(Path(p) for p in manifest['toolchain'] if p.endswith('g++'))
    mesa=Path('/home/liufeng/steamohos-software-gl-20261019-v6')
    wayland=Path('/home/liufeng/steamohos-native-20261018-build/gnu-wayland-v3')
    protocol=a.protocol_dir.resolve()
    inputs={str(p):sha(p) for p in [Path(__file__), source, translation, compiler,
        *sorted((fixture/'native_bc').glob('*')), out/'native_bc_spirv.h', out/'native_bc_sizes.h',
        protocol/'xdg-shell-client-protocol.h', protocol/'xdg-shell-protocol.o', fixture/'native_x11_window.h'] if p.is_file()}
    record=out/'native-bc-runtime.json'; binary=out/'libsteamohos_native_vulkan_bc.so'
    old=json.loads(record.read_text()) if record.exists() else {}
    if old.get('inputs')==inputs and binary.exists() and old.get('sha256')==sha(binary):
        print('Reused isolated native BC WSI'); return
    command=[str(compiler), '-O2','-g0','-fPIC','-Wall','-Wextra','-Werror',
        '-Wno-misleading-indentation','-Wno-missing-field-initializers','-std=c++17','-shared',
        '-I'+str(fixture),'-I'+str(out),'-I'+str(protocol),
        '-I'+str(ROOT/'thirdparty/mesa/include'),'-I'+str(mesa/'deps-root/usr/include'),
        '-I'+str(wayland/'build/gnu-deps/include'),str(translation),str(protocol/'xdg-shell-protocol.o'),
        '-L'+str(wayland/'runtime'),'-L'+str(mesa/'deps-root/usr/lib/aarch64-linux-gnu'),
        '-Wl,-rpath-link,'+str(wayland/'runtime'),'-Wl,-rpath-link,'+str(mesa/'deps-root/usr/lib/aarch64-linux-gnu'),
        '-Wl,-z,relro,-z,now','-l:libwayland-client.so.0','-l:libxcb.so.1','-ldl','-pthread','-o',str(binary)]
    subprocess.run(command,check=True,timeout=120)
    record.write_text(json.dumps(dict(inputs=inputs,command=command,sha256=sha(binary),bytes=binary.stat().st_size,
        experimental=True,releasePayload=False),indent=2)+'\n')
    print('Built isolated BC WSI',binary.stat().st_size,sha(binary))
if __name__=='__main__': main()
