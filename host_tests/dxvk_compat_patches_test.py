"""Exercise the real managed DXVK overlay on pristine pinned sources."""
import hashlib
import io
import json
from pathlib import Path
import re
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import apply_dxvk_compat_patches as overlay


class OverlayTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.source = Path(self.temp.name) / 'source'
        self.source.mkdir()
        self.pin = json.loads((ROOT / 'docs/graphics/texture-compat-runtime.lock.json').read_text())['runtimes']['legacy']
        self.patch = ROOT / self.pin['patches'][0]['path']
        self.names = sorted({name for row in self.pin['patches'] for name in re.findall(r'^diff --git a/(\S+) b/\S+$', (ROOT / row['path']).read_text(), re.M)})
        self.assertTrue(self.names)
        repository = ROOT / 'thirdparty/dxvk'
        present = set(subprocess.check_output(['git', '-C', str(repository), 'ls-tree', '-r',
                                              '--name-only', self.pin['commit']], text=True).splitlines())
        original_names = [name for name in self.names if name in present]
        archive = subprocess.check_output(['git', '-C', str(repository), 'archive', self.pin['commit'], *original_names])
        with tarfile.open(fileobj=io.BytesIO(archive)) as files:
            files.extractall(self.source, filter='data')
        subprocess.run(['git', 'init', '-q', str(self.source)], check=True)

    def hashes(self):
        return {name: hashlib.sha256((self.source / name).read_bytes()).hexdigest()
                if (self.source / name).is_file() else None for name in self.names}

    def test_clean_apply_is_idempotent_and_verifiable(self):
        before = self.hashes()
        with self.assertRaisesRegex(ValueError, 'overlay missing'):
            overlay.apply(self.source, verify_only=True)
        self.assertEqual(overlay.apply(self.source), self.pin['patchedSourceSha256'])
        after = self.hashes()
        self.assertTrue(all(after.values()), 'every overlay file, including added headers, must be produced')
        self.assertNotEqual(before, after)
        overlay.apply(self.source)
        overlay.apply(self.source, verify_only=True)
        self.assertEqual(self.hashes(), after)

    def test_source_drift_is_rejected_without_reset(self):
        overlay.apply(self.source)
        target = self.source / 'src/dxbc/dxbc_compiler.cpp'
        modified = target.read_text().replace('emitBgraUavSwizzle', 'driftedBgraUavSwizzle')
        target.write_text(modified)
        with self.assertRaises(subprocess.CalledProcessError):
            overlay.apply(self.source)
        self.assertEqual(target.read_text(), modified)

    def test_patch_digest_is_checked_before_mutating_source(self):
        root = Path(self.temp.name) / 'lock-root'
        lock = root / 'docs/graphics/texture-compat-runtime.lock.json'
        lock.parent.mkdir(parents=True)
        lock.write_text(json.dumps({'runtimes': {'legacy': self.pin}}))
        payload = root / self.pin['patches'][0]['path']
        payload.parent.mkdir(parents=True)
        payload.write_bytes(self.patch.read_bytes() + b'corrupted\n')
        before = self.hashes()
        with patch.object(overlay, 'ROOT', root), self.assertRaisesRegex(ValueError, 'digest mismatch'):
            overlay.apply(self.source)
        self.assertEqual(self.hashes(), before)


if __name__ == '__main__':
    unittest.main()
