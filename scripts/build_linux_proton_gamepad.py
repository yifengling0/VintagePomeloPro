#!/usr/bin/env python3
"""Build a GNU SDL2 input adapter without editing the shared SDL source tree."""
from pathlib import Path
import argparse,hashlib,json,shutil,subprocess

def sha(path):
    digest=hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda:stream.read(1024*1024),b''):digest.update(chunk)
    return digest.hexdigest()

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source',type=Path,required=True,help='Existing SDL2 source checkout')
    p.add_argument('--cc',type=Path,required=True,help='AArch64 GNU compiler')
    p.add_argument('--output',type=Path,required=True,help='Reusable task directory')
    a=p.parse_args();source=a.source.resolve(strict=True);cc=a.cc.resolve(strict=True)
    output=a.output.resolve();repo=Path(__file__).resolve().parents[1]
    if output.is_relative_to(source) or output.is_relative_to(repo):p.error('Build outside source trees')
    if subprocess.check_output([str(cc),'-dumpmachine'],text=True).strip()!='aarch64-none-linux-gnu':p.error('GNU AArch64 compiler required')
    output.mkdir(parents=True,exist_ok=True)
    adapter=repo/'entry/src/main/cpp/linux_steam/gnu_fixture/proton_gamepad.c'
    protocol=repo/'entry/src/main/cpp/input/controller/gamepad_ipc_protocol.h'
    paths=subprocess.check_output(['rg','--files',str(source)],text=True).splitlines()
    inputs={str(Path(x).relative_to(source)):sha(Path(x)) for x in paths
        if Path(x).suffix in {'.c','.h','.cmake','.in'} or Path(x).name=='CMakeLists.txt'}
    inputs.update({'adapter':sha(adapter),'protocol':sha(protocol),'compiler':sha(cc),'builder':sha(Path(__file__))})
    identity=output/'proton-gamepad-build.json';library=output/'libSDL2-2.0.so.0'
    if identity.exists() and library.exists():
        previous=json.loads(identity.read_text())
        if previous.get('inputs')==inputs and previous.get('sha256')==sha(library):
            print('Reused verified GNU SDL2 ControllerHub adapter');return
    toolchain=output/'gnu-toolchain.cmake';extra=output/'sdl2-proton-extra.cmake'
    toolchain.write_text('set(CMAKE_SYSTEM_NAME Linux)\nset(CMAKE_SYSTEM_PROCESSOR aarch64)\n'
        'set(CMAKE_C_COMPILER '+str(cc)+')\nset(CMAKE_CXX_COMPILER '+str(cc).replace('gcc','g++')+')\n'
        'set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)\nset(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)\n'
        'set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)\nset(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)\n')
    extra.write_text('cmake_language(DEFER CALL target_sources SDL2 PRIVATE '+str(adapter)+')\n')
    options=['-DCMAKE_TOOLCHAIN_FILE='+str(toolchain),'-DCMAKE_PROJECT_SDL2_INCLUDE='+str(extra),
        '-DCMAKE_BUILD_TYPE=Release','-DSDL_SHARED=ON','-DSDL_STATIC=OFF','-DSDL_TEST=OFF','-DSDL_TESTS=OFF',
        '-DSDL_AUDIO=OFF','-DSDL_VIDEO=OFF','-DSDL_RENDER=OFF','-DSDL_POWER=OFF','-DSDL_DBUS=OFF','-DSDL_IBUS=OFF',
        '-DSDL_LIBUDEV=OFF','-DSDL_HIDAPI_JOYSTICK=OFF','-DSDL_VIRTUAL_JOYSTICK=ON','-DSDL_RPATH=OFF',
        '-DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=FALSE','-DPKG_CONFIG_EXECUTABLE=/bin/false']
    build=output/'sdl2-build'
    for name,command in [('sdl2-configure.log',['cmake','-S',str(source),'-B',str(build),*options]),
            ('sdl2-build.log',['cmake','--build',str(build),'-j','4','--target','SDL2'])]:
        with (output/name).open('w') as log:
            result=subprocess.run(command,stdout=log,stderr=subprocess.STDOUT)
        if result.returncode:raise RuntimeError(name+': '+(output/name).read_text()[-4000:])
    compiled=(build/'libSDL2-2.0.so.0').resolve(strict=True)
    if not compiled.is_relative_to(build):raise ValueError('SDL output escaped build directory')
    header=compiled.read_bytes()[:64]
    if header[:4]!=b'\x7fELF' or int.from_bytes(header[18:20],'little')!=183:raise ValueError('Wrong SDL output architecture')
    if not library.exists() or sha(library)!=sha(compiled):shutil.copyfile(compiled,library)
    identity.write_text(json.dumps({'inputs':inputs,'options':options,'bytes':library.stat().st_size,
        'sha256':sha(library),'scope':'GNU Wine private experiment SDL2 input only; not the product SDL or Steam SDL3'},indent=2)+'\n')
    print('GNU SDL2 adapter',library.stat().st_size,'bytes',sha(library))
if __name__=='__main__':main()
