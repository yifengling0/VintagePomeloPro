"""Check the packaged builtin identity used by prefix update reconciliation."""
import hashlib
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'scripts/stamp_wine_builtin_identity.py'
spec = importlib.util.spec_from_file_location('identity', SCRIPT)
identity = importlib.util.module_from_spec(spec)
spec.loader.exec_module(identity)


class BuiltinIdentity(unittest.TestCase):
    def test_refresh_only_for_builtin_changes(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            inf = root / 'share/wine/wine.inf'
            inf.parent.mkdir(parents=True)
            original = b'[Version]\r\nSignature="$CHICAGO$"\r\n'
            inf.write_bytes(original)
            paths = ['bin/aarch64-windows/libwow64fex.dll',
                     'bin/aarch64-windows/libarm64ecfex.dll',
                     'bin/aarch64-windows/ntdll.dll', 'bin/i386-windows/ntdll.dll',
                     'bin/aarch64-unix/ntdll.so']
            for name in paths:
                p = root / name
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_bytes(b'original')
            def run():
                subprocess.run(['python3', str(SCRIPT), str(root)], check=True)
                return inf.read_bytes()
            baseline = run()
            self.assertTrue(baseline.startswith(original))
            before_stat = inf.stat()
            self.assertEqual(run(), baseline)
            self.assertEqual(inf.stat().st_mtime_ns, before_stat.st_mtime_ns)
            (root / paths[-1]).write_bytes(b'graphics/Unix-only update')
            self.assertEqual(run(), baseline)
            for name in paths[:-1]:
                (root / name).write_bytes(b'fixed')
                updated = run()
                self.assertNotEqual(updated, baseline, name)
                self.assertEqual(updated.count(identity.MARKER), 1)
                baseline = updated
            (root / paths[0]).unlink()
            self.assertNotEqual(run(), baseline)

    def test_order_independent_and_empty_rejected(self):
        entries = [('bin/i386-windows/ntdll.dll', hashlib.sha256(b'a').hexdigest()),
                   ('bin/aarch64-windows/ntdll.dll', hashlib.sha256(b'b').hexdigest())]
        self.assertEqual(identity.stamp(b'[Version]\n', entries),
                         identity.stamp(b'[Version]\n', list(reversed(entries))))
        with self.assertRaises(ValueError):
            identity.stamp(b'[Version]\n', [])


if __name__ == '__main__':
    unittest.main()
