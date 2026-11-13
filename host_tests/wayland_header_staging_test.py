"""Exercise guest-gfx header staging from a versioned Wayland build directory."""
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = (ROOT / 'scripts/build_ohos_guest_gfx.sh').read_text()
FUNCTIONS = SCRIPT[SCRIPT.index('copy_if_missing() {'):SCRIPT.index('\nsetup_build_env', SCRIPT.index('copy_if_missing() {'))]


class WaylandHeaderStaging(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='wayland-headers-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        source = self.root / 'thirdparty/wayland'
        (source / 'src').mkdir(parents=True)
        (source / 'egl').mkdir()
        (source / 'meson.build').write_text("project('wayland', 'c',\n  version: '1.26.0',\n)\n")
        for name in ['wayland-client.h', 'wayland-client-core.h', 'wayland-server.h',
                     'wayland-server-core.h', 'wayland-util.h']:
            (source / 'src' / name).write_text(name)
        self.generated = self.root / 'build/wayland_build_1.26.0/aarch64/src'
        self.generated.mkdir(parents=True)
        for name in ['wayland-version.h', 'wayland-client-protocol.h', 'wayland-server-protocol.h']:
            (self.generated / name).write_text('pinned ' + name)
        self.output = self.root / 'sysroot/include'

    def stage(self):
        quote = shlex.quote
        command = ('set -euo pipefail\n'
                   f'ROOT={quote(str(self.root))}\nBUILD_DIR="$ROOT/build"\n'
                   f'SYSROOT_EXT_INC={quote(str(self.output))}\nWINE_ARCH=aarch64\n'
                   'err() { echo "$*" >&2; exit 1; }\n' + FUNCTIONS + '\nensure_wayland_dev_headers\n')
        return subprocess.run(['bash', '-c', command], capture_output=True, text=True, timeout=10)

    def test_clean_versioned_build_needs_only_real_generated_headers(self):
        result = self.stage()
        self.assertEqual(result.returncode, 0, result.stderr)
        for header in ['wayland-client-protocol.h', 'wayland-server-protocol.h', 'wayland-version.h']:
            self.assertEqual((self.output / header).read_text(), 'pinned ' + header)

    def test_missing_server_protocol_rejected(self):
        (self.generated / 'wayland-server-protocol.h').unlink()
        result = self.stage()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('wayland-server-protocol.h is missing', result.stderr)

    def test_stale_unversioned_build_cannot_supply_pinned_headers(self):
        (self.generated / 'wayland-server-protocol.h').unlink()
        stale = self.root / 'build/wayland_build/aarch64/src'
        stale.mkdir(parents=True)
        (stale / 'wayland-server-protocol.h').write_text('stale')
        self.assertNotEqual(self.stage().returncode, 0)


if __name__ == '__main__':
    unittest.main()
