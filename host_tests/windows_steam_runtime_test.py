"""Reject stale source/binary/HAP identities before publishing a helper update."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('windows_steam_runtime', ROOT / 'scripts/prepare_windows_steam_runtime.py')
runtime = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runtime)


class RuntimeIdentityTests(unittest.TestCase):
    def test_shipped_source_and_binary(self):
        self.assertEqual(runtime.verify()['helper'], 'vpsteamtrust32.dll')

    def test_stale_source_and_binary_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for name in (runtime.SOURCE, runtime.ASSET + '/manifest.json', runtime.ASSET + '/vpsteamtrust32.dll'):
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes((ROOT / name).read_bytes())
            source = root / runtime.SOURCE
            source.write_bytes(source.read_bytes() + b'\n')
            with self.assertRaisesRegex(ValueError, 'source changed'):
                runtime.verify(root)
            source.write_bytes((ROOT / runtime.SOURCE).read_bytes())
            payload = root / runtime.ASSET / 'vpsteamtrust32.dll'
            payload.write_bytes(b'wrong helper')
            with self.assertRaisesRegex(ValueError, 'payload identity'):
                runtime.verify(root)

    def test_hap_must_ship_exact_resources(self):
        with tempfile.TemporaryDirectory() as tmp:
            hap = Path(tmp) / 'candidate.hap'
            for stale in (False, True):
                with zipfile.ZipFile(hap, 'w') as archive:
                    for name in ('manifest.json', 'vpsteamtrust32.dll'):
                        data = (ROOT / runtime.ASSET / name).read_bytes()
                        archive.writestr('resources/rawfile/windows-steam/' + name,
                                         b'stale' if stale and name.endswith('.dll') else data)
                if stale:
                    with self.assertRaisesRegex(ValueError, 'stale Windows Steam resource'):
                        runtime.verify(hap=hap)
                else:
                    runtime.verify(hap=hap)


if __name__ == '__main__':
    unittest.main()
