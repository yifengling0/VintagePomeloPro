"""Catch missing native locks and source files before a long CI build."""
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from ci_preflight import package_inputs


class InputTests(unittest.TestCase):
    def test_missing_lock_and_untracked_native_type_are_both_reported(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            def write(relative, data):
                p = root / relative
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_text(json.dumps(data))
            write('oh-package.json5', {})
            write('oh-package-lock.json5', {'specifiers': {}, 'packages': {}})
            write('entry/oh-package.json5', {'dependencies': {'libtest.so': 'file:./types/test'}})
            lock = {'specifiers': {}, 'packages': {}}
            write('entry/oh-package-lock.json5', lock)
            write('entry/types/test/oh-package.json5', {'name': 'libtest.so', 'version': '1.0.0'})
            write('entry/types/test/Index.d.ts', {})
            write('docs/linux-steam/build-components.lock.json', {'sourceInputs': {}})
            with self.assertRaisesRegex(ValueError, 'missing lock entry') as error:
                package_inputs(root, set())
            self.assertIn('Uncommitted native type input', str(error.exception))
            key = 'libtest.so@types/test'
            lock['specifiers'][key] = key
            lock['packages'][key] = {'name': 'libtest.so', 'version': '1.0.0', 'resolved': 'types/test'}
            write('entry/oh-package-lock.json5', lock)
            tracked = {p.relative_to(root).as_posix() for p in root.rglob('*') if p.is_file()}
            package_inputs(root, tracked)
            with self.assertRaisesRegex(ValueError, 'Uncommitted native type input'):
                package_inputs(root, tracked - {'entry/types/test/Index.d.ts'})


if __name__ == '__main__':
    unittest.main()
