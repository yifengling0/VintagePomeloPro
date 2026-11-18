#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Exercise reviewed recipe refresh against a tiny real patchable tree."""
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'scripts'))
import gamescope_source as source


def sha(data):
    return hashlib.sha256(data.encode()).hexdigest()


class RecipeTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root/'components').mkdir()
        (self.root/'pristine').mkdir()
        (self.root/'pristine/a').write_text('base\n')
        (self.root/'pristine/untouched').write_text('keep\n')
        (self.root/'overlay').write_text('overlay\n')
        self.work = self.root/'work'; self.work.mkdir()
        (self.root/'components/source.json').write_text('{}')
        self.recipe = {'sourceLock': 'components/source.json', 'patches': [], 'overlays': []}
        self.patchers = [patch.object(source, 'ROOT', self.root),
                         patch.object(source, 'prepare', return_value=self.root/'pristine')]
        for p in self.patchers:
            p.start(); self.addCleanup(p.stop)
        self.write_recipe('first')
        self.target = source.prepare_recipe(self.root, self.work)

    def write_recipe(self, value, overlay='added'):
        text = '--- a/a\n+++ b/a\n@@ -1 +1 @@\n-base\n+'+value+'\n'
        (self.root/'change.patch').write_text(text)
        self.recipe['patches'] = [{'path': 'change.patch', 'sha256': sha(text),
            'originalPath': 'a', 'beforeSha256': sha('base\n'), 'afterSha256': sha(value+'\n')}]
        self.recipe['overlays'] = [{'source': 'overlay', 'destination': overlay, 'sha256': sha('overlay\n')}]
        (self.root/'components/gamescope-ohos.lock.json').write_text(json.dumps(self.recipe))

    def test_same_input_reuses_source(self):
        before = (self.target/'a').stat().st_mtime_ns
        self.assertEqual(source.prepare_recipe(self.root, self.work), self.target)
        self.assertEqual((self.target/'a').stat().st_mtime_ns, before)

    def test_reviewed_refresh_retires_overlay(self):
        self.write_recipe('second', overlay='new-overlay')
        source.prepare_recipe(self.root, self.work, refresh=True)
        self.assertEqual((self.target/'a').read_text(), 'second\n')
        self.assertFalse((self.target/'added').exists())
        self.assertEqual((self.target/'new-overlay').read_text(), 'overlay\n')
        self.assertEqual((self.target/'untouched').read_text(), 'keep\n')
        source.prepare_recipe(self.root, self.work)

    def test_unknown_edit_is_never_reset(self):
        (self.target/'untouched').write_text('user edit\n')
        self.write_recipe('second')
        with self.assertRaisesRegex(ValueError, 'preserved'):
            source.prepare_recipe(self.root, self.work, refresh=True)
        self.assertEqual((self.target/'untouched').read_text(), 'user edit\n')
        self.assertEqual((self.target/'a').read_text(), 'first\n')

    def test_bad_patch_preserves_receipt_and_source(self):
        receipt = self.work/'source-receipt.json'; old = receipt.read_bytes()
        self.write_recipe('second')
        self.recipe['patches'][0]['afterSha256'] = sha('wrong\n')
        (self.root/'components/gamescope-ohos.lock.json').write_text(json.dumps(self.recipe))
        with self.assertRaisesRegex(ValueError, 'After-patch'):
            source.prepare_recipe(self.root, self.work, refresh=True)
        self.assertEqual(receipt.read_bytes(), old)
        self.assertEqual((self.target/'a').read_text(), 'first\n')

    def test_publish_failure_rolls_back_all_touched_files(self):
        receipt = self.work/'source-receipt.json'; old = receipt.read_bytes()
        self.write_recipe('second', overlay='new-overlay')
        original = source.shutil.copy2
        def fail(src, dst, *args, **kwargs):
            if Path(dst) == self.target/'new-overlay':
                raise OSError('simulated publication failure')
            return original(src, dst, *args, **kwargs)
        with patch.object(source.shutil, 'copy2', side_effect=fail):
            with self.assertRaisesRegex(OSError, 'publication'):
                source.prepare_recipe(self.root, self.work, refresh=True)
        self.assertEqual(receipt.read_bytes(), old)
        self.assertEqual((self.target/'a').read_text(), 'first\n')
        self.assertEqual((self.target/'added').read_text(), 'overlay\n')
        self.assertFalse((self.target/'new-overlay').exists())

    def test_changed_patch_requires_explicit_refresh(self):
        self.write_recipe('second')
        with self.assertRaisesRegex(ValueError, 'preserved'):
            source.prepare_recipe(self.root, self.work)
        self.assertEqual((self.target/'a').read_text(), 'first\n')

    def test_receipt_replace_failure_keeps_previous_identity(self):
        receipt = self.work/'source-receipt.json'; old = receipt.read_bytes()
        self.write_recipe('second')
        with patch.object(Path, 'replace', side_effect=OSError('receipt publish failed')):
            with self.assertRaisesRegex(OSError, 'receipt publish'):
                source.prepare_recipe(self.root, self.work, refresh=True)
        self.assertEqual(receipt.read_bytes(), old)
        self.assertEqual((self.target/'a').read_text(), 'first\n')
        self.assertFalse(receipt.with_suffix('.pending').exists())


if __name__ == '__main__':
    unittest.main()
