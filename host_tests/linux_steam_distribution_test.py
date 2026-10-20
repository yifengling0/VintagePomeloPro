"""Small adversarial fixtures for the shipping distribution builder."""
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import zipfile

spec = importlib.util.spec_from_file_location('distribution', Path(__file__).parents[1] / 'scripts/build_linux_steam_distribution.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
offline_spec = importlib.util.spec_from_file_location('offline', Path(__file__).parents[1] / 'scripts/build_linux_steam_offline_package.py')
offline = importlib.util.module_from_spec(offline_spec)
offline_spec.loader.exec_module(offline)


class DistributionTests(unittest.TestCase):
    def test_paths(self):
        for path in ('../escape', '/absolute', 'a/../b', 'a//b', 'a\\b', 'a:drive', 'a\x00b'):
            with self.subTest(path=path), self.assertRaises(ValueError):
                module.safe(path)
        self.assertEqual(module.safe('steamrtarm64/libs/a.so'), 'steamrtarm64/libs/a.so')

    def fixture(self, root, extra=None, unmapped=False):
        runtime = root / 'runtime'
        downloads = root / 'downloads'
        runtime.mkdir(); downloads.mkdir()
        members = {'steamrtarm64\\steam': b'officialELF', 'steamui\\index.html': b'officialHTML',
                   'steamrtarm64/icudtl.dat': b'officialICU', 'bin/d3ddriverquery64.exe': b'officialD3D',
                   'legacycompat/iscriptevaluator.exe': b'officialSCRIPT'}
        archive = downloads / 'official.zip'
        with zipfile.ZipFile(archive, 'w') as out:
            for name, data in members.items():
                out.writestr(name, data)
            if extra:
                out.writestr(*extra)
        manifest = {'steamClientProbe': {'helpers': {'provenance': {
            'package': archive.name, 'packageSha1': hashlib.sha1(archive.read_bytes()).hexdigest()}}}}
        (runtime / 'manifest.json').write_text(json.dumps(manifest))
        (runtime / 'gnu.so').write_bytes(b'self-maintained GNU')
        (runtime / 'steamrtarm64').mkdir()
        (runtime / 'steamrtarm64/steam').write_bytes(b'officialELF')
        with zipfile.ZipFile(runtime / 'steam-client-data.zip', 'w') as out:
            out.writestr('steamui/index.html', b'changedValve' if unmapped else b'officialHTML')
            out.writestr('steamrtarm64/icudtl.dat', b'officialICU')
            out.writestr('steamohos/ca-certificates.crt', b'CA')
        with zipfile.ZipFile(runtime / 'steam-client-helpers.zip', 'w') as out:
            out.writestr('bin/d3ddriverquery64.exe', b'officialD3D')
            out.writestr('legacycompat/iscriptevaluator.exe', b'officialSCRIPT')
        lock = root / 'lock.json'; lock.write_text(json.dumps({'version':'1', 'packages': {}}))
        return runtime, lock, downloads, root / 'output', 'rawfile://linux-steam/hmosdeck-gnu-arm64-v1.zip'

    def test_proprietary_separation_and_reproducibility(self):
        with tempfile.TemporaryDirectory() as t:
            args = self.fixture(Path(t))
            first = module.build(*args)
            second = module.build(*args)
            self.assertEqual(first, second)
            plan = json.loads((args[3] / 'deployment.json').read_text())
            with zipfile.ZipFile(args[3] / 'hmosdeck-gnu-arm64-v1.zip') as archive:
                contents = b''.join(archive.read(n) for n in archive.namelist())
                self.assertNotIn(b'officialELF', contents)
                self.assertNotIn(b'officialHTML', contents)
                self.assertIn(b'self-maintained GNU', contents)
            elf = next(f for f in plan['files'] if f['path'].endswith('/steam'))
            self.assertEqual(elf['source'], 'steamrtarm64/steam')
            self.assertEqual(elf['zipPath'], 'steamrtarm64\\steam')
            self.assertEqual(plan['downloadBytes'], plan['archives'][1]['bytes'])

    def test_unmapped_valve_bytes_are_rejected(self):
        with tempfile.TemporaryDirectory() as t:
            with self.assertRaisesRegex(ValueError, 'Unmapped official'):
                module.build(*self.fixture(Path(t), unmapped=True))

    def test_offline_bundle_is_pinned_reproducible_and_rejects_tampering(self):
        with tempfile.TemporaryDirectory() as t:
            args = self.fixture(Path(t))
            module.build(*args)
            output = Path(t) / 'offline.zip'
            metadata = Path(t) / 'metadata'
            first = offline.build(args[3] / 'deployment.json', args[2], output, metadata)
            second = offline.build(args[3] / 'deployment.json', args[2], output, metadata)
            self.assertEqual(first, second)
            self.assertEqual(first['sha256'], hashlib.sha256(output.read_bytes()).hexdigest())
            with zipfile.ZipFile(output) as archive:
                names = archive.namelist()
                self.assertEqual(len(names), 2)
                self.assertIn('bundle-info.json', names)
                self.assertNotIn(b'self-maintained GNU', b''.join(archive.read(n) for n in names))
            (args[2] / 'official.zip').write_bytes(b'tampered')
            with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
                offline.build(args[3] / 'deployment.json', args[2], output, metadata)
            self.assertEqual(first['sha256'], hashlib.sha256(output.read_bytes()).hexdigest())

    def test_zip_traversal_rejected(self):
        with tempfile.TemporaryDirectory() as t:
            with self.assertRaisesRegex(ValueError, 'Unsafe relative path'):
                module.build(*self.fixture(Path(t), extra=('../escape', b'bad')))

    def test_zip_link_escape_rejected(self):
        with tempfile.TemporaryDirectory() as t:
            info = zipfile.ZipInfo('link'); info.create_system = 3; info.external_attr = 0o120777 << 16
            with self.assertRaisesRegex(ValueError, 'Unsafe relative path'):
                module.build(*self.fixture(Path(t), extra=(info, b'../escape')))


if __name__ == '__main__':
    unittest.main()
