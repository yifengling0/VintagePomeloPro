"""Prove pristine wlroots protocol includes are generated from pinned XML."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from prepare_wlroots_protocol_headers import prepare


class WlrootsProtocolHeadersTest(unittest.TestCase):
    def test_clean_generation_and_host_compilation(self):
        scanner = Path(os.environ.get('WAYLAND_SCANNER', ROOT / 'build/host-tools/bin/wayland-scanner'))
        self.assertTrue(scanner.is_file(), 'the pinned native wayland-scanner is required')
        with tempfile.TemporaryDirectory() as directory:
            include = Path(directory) / 'include'
            names = prepare(ROOT / 'thirdparty/wlroots', ROOT / 'thirdparty/wayland-protocols',
                            scanner, include / 'wayland-protocols')
            self.assertIn('ext-image-copy-capture-v1', names)
            headers = ''.join('#include <wayland-protocols/' + name + '-enum.h>\n' for name in names)
            code = headers + 'enum ext_image_copy_capture_frame_v1_failure_reason reason;\n'
            subprocess.run(['cc', '-x', 'c', '-std=c11', '-Werror', '-fsyntax-only', '-I', str(include), '-'],
                           input=code, text=True, check=True)

    def test_missing_xml_does_not_replace_existing_header(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'wlroots/include/wlr').mkdir(parents=True)
            (root / 'wlroots/include/wlr/test.h').write_text('#include <wayland-protocols/missing-enum.h>\n')
            (root / 'protocols').mkdir()
            out = root / 'output'; out.mkdir()
            original = out / 'existing-enum.h'; original.write_text('preserve')
            with self.assertRaisesRegex(ValueError, 'Expected one pinned protocol XML'):
                prepare(root / 'wlroots', root / 'protocols', Path('/unused'), out)
            self.assertEqual(original.read_text(), 'preserve')

if __name__ == '__main__':
    unittest.main()
