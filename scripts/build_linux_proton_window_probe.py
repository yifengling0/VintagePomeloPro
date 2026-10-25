#!/usr/bin/env python3
"""Build the isolated Windows x64 window fixture and upstream Wayland EGL ABI helper."""
from pathlib import Path
import argparse, hashlib, json, subprocess

def sha(path):
    digest = hashlib.sha256()
    with path.open('rb') as f:
        for chunk in iter(lambda: f.read(1024*1024), b''): digest.update(chunk)
    return digest.hexdigest()

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--windows-cc', type=Path, required=True)
p.add_argument('--gnu-cc', type=Path, required=True)
p.add_argument('--wayland-source', type=Path, required=True)
p.add_argument('--wayland-include', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
root = Path(__file__).resolve().parents[1]
output = a.output.resolve()
source = a.wayland_source.resolve(strict=True)
include = a.wayland_include.resolve(strict=True)
if output.is_relative_to(root) or output.is_relative_to(source):
    p.error('Use a reusable task directory outside the source trees')
output.mkdir(parents=True, exist_ok=True)
cc, win = a.gnu_cc.resolve(strict=True), a.windows_cc.resolve(strict=True)
assert subprocess.check_output([str(cc), '-dumpmachine'], text=True).strip().startswith('aarch64')
assert subprocess.check_output([str(win), '-dumpmachine'], text=True).strip() == 'x86_64-w64-mingw32'
fixture = root/'entry/src/main/cpp/linux_steam/gnu_fixture/proton_window.c'
launcher_source = fixture.with_name('proton_window_launcher.c')
media_source = fixture.with_name('proton_media.c')
inputs = {str(path):sha(path) for path in [fixture, launcher_source, media_source, cc, win, source/'egl/wayland-egl.c',
    source/'egl/wayland-egl-backend.h', source/'egl/wayland-egl.h',
    include/'wayland-version.h', include/'wayland-client-core.h', include/'wayland-util.h']}
identity = output/'window-build-identity.json'
previous = json.loads(identity.read_text()) if identity.exists() else {}
# Each output has its own dependency set. A window-only change must not
# regenerate unrelated media binaries (including their PE timestamps).
output_inputs = {
    'proton-x64-window.exe': [fixture, win],
    'proton-x64-window-launcher.exe': [launcher_source, win],
    'proton-x64-media.exe': [media_source, win],
    'libwayland-egl.so.1': [cc, source/'egl/wayland-egl.c', source/'egl/wayland-egl-backend.h',
                           source/'egl/wayland-egl.h', include/'wayland-version.h',
                           include/'wayland-client-core.h', include/'wayland-util.h'],
}
def rebuild(name):
    target = output/name
    required = {str(path): inputs[str(path)] for path in output_inputs[name]}
    old = previous.get('perOutputInputs', {}).get(name,
        {key:previous.get('inputs', {}).get(key) for key in required})
    return (old != required or not target.is_file() or
            previous.get('outputs', {}).get(name) != sha(target))
window = output/'proton-x64-window.exe'
egl = output/'libwayland-egl.so.1'
launcher = output/'proton-x64-window-launcher.exe'
media = output/'proton-x64-media.exe'
if rebuild('proton-x64-window.exe'):
    subprocess.run([str(win), '-O2', '-Wall', '-Wextra', '-Werror', '-static-libgcc', str(fixture),
        '-o', str(window), '-luser32', '-lgdi32'], check=True)
if rebuild('proton-x64-window-launcher.exe'):
    subprocess.run([str(win), '-O2', '-Wall', '-Wextra', '-Werror', '-static-libgcc', str(launcher_source), '-o', str(launcher)], check=True)
if rebuild('libwayland-egl.so.1'):
    subprocess.run([str(cc), '-O2', '-shared', '-fPIC', '-I'+str(include), '-I'+str(source/'egl'),
        str(source/'egl/wayland-egl.c'), '-Wl,-soname,libwayland-egl.so.1', '-o', str(egl)], check=True)
if rebuild('proton-x64-media.exe'):
    subprocess.run([str(win), '-O2', '-Wall', '-Wextra', '-Werror', '-static-libgcc', str(media_source),
        '-o', str(media), '-luser32', '-lgdi32', '-lopengl32', '-lwinmm', '-lole32', '-luuid', '-lm'], check=True)
identity.write_text(json.dumps({'inputs':inputs, 'perOutputInputs':{name:{str(path):inputs[str(path)] for path in deps} for name,deps in output_inputs.items()}, 'outputs':{path.name:sha(path) for path in [window,egl,launcher,media]},
    'claim':'GDI window diagnostic and Wayland EGL window ABI; does not supply a GPU driver'}, indent=2)+'\n')
print(identity.read_text())
