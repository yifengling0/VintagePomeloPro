"""Real ZIP publication compatibility; no device or user library data."""
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('offline', ROOT / 'scripts/build_linux_steam_offline_package.py')
offline = importlib.util.module_from_spec(spec)
spec.loader.exec_module(offline)


class OfflinePackageTest(unittest.TestCase):
    def test_host_updates_reuse_bytes_and_validate_legacy_publications(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            downloads = root / 'downloads'
            downloads.mkdir()
            rawfile = root / 'rawfile'
            payload = downloads / 'official.zip'
            with zipfile.ZipFile(payload, 'w') as archive:
                archive.writestr('client', b'official-client-fixture')
            checksum = offline.file_hash(payload)
            plan = {'id': 'a' * 64, 'clientBuild': '123', 'archives': [
                {'id': 'official', 'url': 'https://example.invalid/official.zip',
                 'bytes': payload.stat().st_size, 'sha256': checksum},
                {'id': 'gnu', 'url': 'rawfile://gnu.zip'}]}
            plan_file = root / 'deployment.json'
            plan_file.write_text(json.dumps(plan))
            output = root / 'offline.zip'
            first = offline.build(plan_file, downloads, output, rawfile)
            original = output.read_bytes()
            mtime = output.stat().st_mtime_ns
            plan['id'] = 'b' * 64
            plan_file.write_text(json.dumps(plan))
            second = offline.build(plan_file, downloads, output, rawfile)
            self.assertEqual(first['sha256'], second['sha256'])
            self.assertEqual(output.read_bytes(), original)
            self.assertEqual(output.stat().st_mtime_ns, mtime)
            legacy = root / 'previous-publication.zip'
            with zipfile.ZipFile(legacy, 'w') as archive:
                for name, data in [('bundle-info.json', b'{"id":"old-host"}'),
                                   ('archives/' + checksum + '.zip', payload.read_bytes())]:
                    entry = zipfile.ZipInfo(name)
                    entry.external_attr = 0o100644 << 16
                    archive.writestr(entry, data)
            third = offline.build(plan_file, downloads, output, rawfile, [legacy])
            self.assertEqual(third['compatiblePackages'], [
                {'bytes': legacy.stat().st_size, 'sha256': offline.file_hash(legacy)}])
            fourth = offline.build(plan_file, downloads, output, rawfile)
            self.assertEqual(fourth['compatiblePackages'], third['compatiblePackages'])
            with zipfile.ZipFile(legacy, 'a') as archive:
                archive.writestr('../unexpected', b'never-extract')
            with self.assertRaises(ValueError):
                offline.build(plan_file, downloads, output, rawfile, [legacy])
            self.assertEqual(output.read_bytes(), original)
            # A changed official payload is a real compatibility boundary.
            with zipfile.ZipFile(payload, 'w') as archive:
                archive.writestr('client', b'new-official-client-fixture')
            plan['archives'][0].update(bytes=payload.stat().st_size, sha256=offline.file_hash(payload))
            plan_file.write_text(json.dumps(plan))
            changed = offline.build(plan_file, downloads, output, rawfile)
            self.assertNotEqual(changed['archiveSetId'], first['archiveSetId'])
            self.assertEqual(changed['compatiblePackages'], [])
            self.assertNotEqual(output.read_bytes(), original)


if __name__ == '__main__':
    unittest.main()
