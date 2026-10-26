#!/usr/bin/env python3
"""Run the real scanner/build functions and compile CMake's chosen headers.

Uses pinned Wayland inputs in a recursive checkout, or system development files
for a host-only regression. Only small protocol/header fixtures are copied.
"""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ProtocolBuildTests(unittest.TestCase):
    def test_generated_protocols_preserve_sources_and_cmake_selects_matching_headers(self):
        source = ROOT / 'thirdparty/wayland'
        if not (source / 'protocol/wayland.xml').exists():
            source = Path('/usr')
            xml = source / 'share/wayland/wayland.xml'
            headers = source / 'include'
        else:
            xml = source / 'protocol/wayland.xml'
            headers = source / 'src'
        scanner = os.environ.get('WAYLAND_SCANNER') or str(ROOT / 'build/host-tools/bin/wayland-scanner')
        if not Path(scanner).is_file():
            scanner = shutil.which('wayland-scanner')
        self.assertTrue(scanner and xml.is_file(), 'build host tools or install Wayland development files first')
        with tempfile.TemporaryDirectory(prefix='native-protocols-') as folder:
            root = Path(folder)
            cpp = root / 'entry/src/main/cpp'
            tracked = cpp / 'protocols'
            tracked.mkdir(parents=True)
            generated = root / 'build/native_arm64-v8a'
            output = generated / 'protocols'
            output.mkdir(parents=True)
            wayland = root / 'thirdparty/wayland'
            (wayland / 'protocol').mkdir(parents=True)
            (wayland / 'src').mkdir()
            shutil.copyfile(xml, wayland / 'protocol/wayland.xml')
            for name in ('wayland-server-core.h', 'wayland-server.h', 'wayland-client-core.h',
                         'wayland-client.h', 'wayland-util.h'):
                shutil.copyfile(headers / name, wayland / 'src' / name)
            wlbuild = generated / 'wayland/src'
            wlbuild.mkdir(parents=True)
            for mode in ('server', 'client'):
                subprocess.run([scanner, mode + '-header', str(xml),
                    str(wlbuild / ('wayland-' + mode + '-protocol.h'))], check=True, capture_output=True)
            # The version header normally comes from Meson's configure step.
            (wlbuild / 'wayland-version.h').write_text('#define WAYLAND_VERSION_MAJOR 1\n')
            xdg = root / 'thirdparty/wayland-protocols/stable/xdg-shell/xdg-shell.xml'
            xdg.parent.mkdir(parents=True)
            xdg.write_text('<protocol name="xdg_shell"><interface name="xdg_wm_base" version="1">'
                           '<request name="destroy" type="destructor"/></interface></protocol>')
            shutil.copyfile(ROOT / 'entry/src/main/cpp/protocols/winehua-toplevel.xml',
                            tracked / 'winehua-toplevel.xml')
            # A wrong include order must fail compilation, even if both bare
            # and protocols/header.h include spellings are used.
            for name in ('xdg-shell-client-protocol.h', 'winehua-toplevel-server-protocol.h'):
                (tracked / name).write_text('#error tracked snapshot selected instead of generated header\n')
            script = (ROOT / 'scripts/build_native.sh').read_text()
            functions = script[script.index('build_protocols() {'):script.index('# ── 5. libepoxy')]
            env = dict(os.environ, ROOT=str(root), NATIVE_BUILD=str(generated),
                       WINEHUA_INC=str(output), WINEHUA_PROTOCOL_SOURCE=str(tracked),
                       WAYLAND_SCANNER=scanner)
            def build_protocols():
                subprocess.run(['bash', '-euo', 'pipefail', '-c',
                    'log() { :; }\n' + functions + '\nbuild_protocols\ninstall_headers\n'],
                    env=env, check=True, capture_output=True)
            def hashes(path):
                return {str(p.relative_to(path)): hashlib.sha256(p.read_bytes()).hexdigest()
                        for p in path.rglob('*') if p.is_file()}
            before = hashes(tracked)
            build_protocols()
            first = hashes(output)
            build_protocols()
            self.assertEqual(first, hashes(output))
            (output / 'xdg-shell-client-protocol.h').unlink()
            build_protocols()
            self.assertEqual(first, hashes(output))
            self.assertEqual(before, hashes(tracked))
            cmake = root / 'cmake-fixture'
            cmake.mkdir()
            (cmake / 'probe.c').write_text('#include "protocols/xdg-shell-client-protocol.h"\n'
                '#include "winehua-toplevel-server-protocol.h"\n'
                'int probe(void) { return sizeof(struct wl_message); }\n')
            helper = ROOT / 'entry/src/main/cpp/cmake/WaylandProtocols.cmake'
            (cmake / 'CMakeLists.txt').write_text(f"""cmake_minimum_required(VERSION 3.16)
project(ProtocolProbe C)
set(NATIVERENDER_ROOT_PATH "{cpp}")
set(OHOS_ARCH arm64-v8a CACHE STRING "")
include("{helper}")
include_directories("{cpp}" "{tracked}")
file(WRITE "${{CMAKE_BINARY_DIR}}/selected.txt" "${{VPP_WAYLAND_PROTOCOLS}}")
add_library(probe OBJECT probe.c "${{VPP_WAYLAND_PROTOCOLS}}/xdg-shell-protocol.c"
    "${{VPP_WAYLAND_PROTOCOLS}}/winehua-toplevel-protocol.c")
""")
            for name, extra in (('default', []), ('custom', ['-DOHOS_ARCH=x86_64', '-DVPP_NATIVE_GENERATED_ROOT=' + str(generated)])):
                build = root / ('cmake-' + name)
                subprocess.run(['cmake', '-S', str(cmake), '-B', str(build), *extra],
                               check=True, capture_output=True)
                self.assertEqual((build / 'selected.txt').read_text(), str(output))
                subprocess.run(['cmake', '--build', str(build)], check=True, capture_output=True)
            # No generated tree: existing prebuilt-only DevEco builds retain
            # their committed protocol sources.
            for name in ('xdg-shell-protocol.c', 'winehua-toplevel-protocol.c'):
                shutil.copyfile(output / name, tracked / name)
            fallback = root / 'cmake-fallback'
            subprocess.run(['cmake', '-S', str(cmake), '-B', str(fallback),
                            '-DVPP_NATIVE_GENERATED_ROOT=' + str(root / 'absent')],
                           check=True, capture_output=True)
            self.assertEqual((fallback / 'selected.txt').read_text(), str(tracked))


if __name__ == '__main__':
    unittest.main()
