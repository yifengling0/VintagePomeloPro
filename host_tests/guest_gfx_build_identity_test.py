"""Reject relabelled VirGL builds and mismatched Zink packaging inputs."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / 'scripts/guest_gfx_build_identity.py'


class GuestGraphicsIdentityTest(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.install = self.root / 'install'
        self.build = self.root / 'build'
        (self.install / 'lib').mkdir(parents=True)
        (self.build / 'meson-info').mkdir(parents=True)
        self.library = self.install / 'lib/libgallium-25.0.1.so'
        self.library.write_bytes(b'fixture - driver bundle A')

    def cli(self, action, arch='aarch64'):
        command = [sys.executable, str(SCRIPT), action, '--install-root', str(self.install), '--arch', arch]
        if action == 'record': command += ['--build-root', str(self.build)]
        return subprocess.run(command, capture_output=True, text=True)

    def record(self, drivers):
        options = {'gallium-drivers': drivers, 'opengl': True, 'platforms': ['wayland']}
        (self.build / 'meson-info/intro-buildoptions.json').write_text(json.dumps([
            {'name': key, 'value': value} for key, value in options.items()]))
        result = self.cli('record')
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_missing_identity_rejected(self):
        self.assertNotEqual(self.cli('verify-zink').returncode, 0)

    def test_renamed_virgl_driver_rejected(self):
        self.record(['virgl', 'softpipe'])
        (self.install / 'lib/dri').mkdir()
        (self.install / 'lib/dri/zink_dri.so').write_bytes(self.library.read_bytes())
        self.assertNotEqual(self.cli('verify-zink').returncode, 0)

    def test_recorded_zink_artifact_accepted(self):
        self.record(['zink', 'virgl', 'softpipe'])
        result = self.cli('verify-zink')
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_wrong_architecture_rejected(self):
        self.record(['zink', 'virgl', 'softpipe'])
        self.assertNotEqual(self.cli('verify-zink', 'x86_64').returncode, 0)

    def test_stale_library_rejected(self):
        self.record(['zink', 'virgl', 'softpipe'])
        self.library.write_bytes(b'fixture - rebuilt driver bundle B')
        self.assertNotEqual(self.cli('verify-zink').returncode, 0)

    def test_symlink_to_unrecorded_external_artifact_rejected(self):
        self.record(['zink', 'virgl', 'softpipe'])
        external = self.root / 'unrelated.so'
        external.write_bytes(self.library.read_bytes())
        self.library.unlink()
        self.library.symlink_to(external)
        self.assertNotEqual(self.cli('verify-zink').returncode, 0)


if __name__ == '__main__':
    unittest.main()
