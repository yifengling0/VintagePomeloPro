"""Verify real ZIP inputs, managed helper upgrades and stale HAP rejection."""
import hashlib
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
from zipfile import ZipFile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import prepare_windows_steam_offline as offline
from prepare_windows_steam_runtime import RESOURCES


def digest(data):
    return hashlib.sha256(data).hexdigest()


class OfflineTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='vpp-windows-offline-')
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.resources = self.root / 'entry/src/main/resources/rawfile/windows-steam'
        self.resources.mkdir(parents=True)
        source = self.root / 'scripts/steam_legacy_zstd/vpsteam_runtime32.c'
        source.parent.mkdir(parents=True)
        source.write_bytes(b'source')
        helper = bytearray(128)
        helper[:2] = b'MZ'
        struct.pack_into('<I', helper, 0x3c, 64)
        helper[64:68] = b'PE\0\0'
        struct.pack_into('<H', helper, 68, 0x14c)
        (self.resources / 'vpsteamtrust32.dll').write_bytes(helper)
        (self.resources / 'library-network.js').write_bytes(b'policy')
        (self.resources / 'library-network.json').write_text(json.dumps(dict(scriptSha256=digest(b'policy'))))
        self.identity = dict(sourceSha256=digest(b'source'), helperSha256=digest(helper),
                             size=len(helper), previousHelperSha256=digest(b'previous helper'),
                             files=[dict(name='steam.exe', sha256=digest(b'client'))])
        (self.resources / 'manifest.json').write_text(json.dumps(self.identity))
        self.archive = self.root / 'WindowsSteam.zip'
        self.entries = {name: b'resource' for name in offline.REQUIRED}
        self.entries.update({'steam.exe': b'client', 'vpsteamtrust32.dll': b'previous helper'})
        self.write_archive()

    def write_archive(self, extra=None):
        entries = dict(self.entries, **(extra or {}))
        entries['SHA256SUMS.txt'] = ''.join(digest(data) + '  ' + name + '\n'
                                          for name, data in entries.items()).encode()
        with ZipFile(self.archive, 'w') as archive:
            for name, data in entries.items():
                archive.writestr('Steam/' + name, data)
        lock = dict(fileName=self.archive.name, bytes=self.archive.stat().st_size,
                    sha256=offline.file_hash(self.archive), files=len(entries),
                    expandedBytes=sum(map(len, entries.values())),
                    url='https://github.com/yifengling0/VintagePomeloPro/releases/download/test/WindowsSteam.zip')
        path = self.root / offline.LOCK
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(lock))

    def write_hap(self, stale=False):
        path = self.root / 'entry.hap'
        with ZipFile(path, 'w') as hap:
            for name in RESOURCES:
                hap.writestr('resources/rawfile/windows-steam/' + name,
                             b'stale helper' if stale and name.endswith('.dll') else
                             (self.resources / name).read_bytes())
        return path

    def test_verified_archive_is_reused_without_network(self):
        with patch.object(offline.urllib.request, 'urlopen', side_effect=AssertionError('must reuse')):
            self.assertEqual(offline.prepare(self.root, self.root), self.archive)
            result = offline.verify_archive(self.root, self.archive, hap=self.write_hap())
        self.assertEqual(result['packages'][0]['kind'], 'Windows Steam')
        self.assertEqual(result['helperSha256'], self.identity['helperSha256'])
        self.assertEqual(len(result['packages']), 1)

    def test_corrupt_archive_is_rejected_and_preserved(self):
        data = self.archive.read_bytes() + b'wrong'
        self.archive.write_bytes(data)
        with self.assertRaisesRegex(ValueError, 'identity mismatch'):
            offline.verify_archive(self.root, self.archive)
        self.assertEqual(self.archive.read_bytes(), data)

    def test_private_and_traversing_entries_are_rejected(self):
        for name in ('../escape', 'config/loginusers.vdf', 'userdata/account', 'ssfn123', 'bin/../escape'):
            with self.subTest(name=name):
                self.write_archive({name: b'forbidden'})
                with self.assertRaisesRegex(ValueError, 'Unsafe|Private'):
                    offline.verify_archive(self.root, self.archive)

    def test_foreign_helper_and_dependencies_are_rejected(self):
        for name in ('vpsteamtrust32.dll', 'steam.exe'):
            with self.subTest(name=name):
                self.write_archive({name: b'foreign'})
                with self.assertRaisesRegex(ValueError, 'helper|dependency'):
                    offline.verify_archive(self.root, self.archive)

    def test_stale_hap_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'stale Windows Steam resource'):
            offline.verify_archive(self.root, self.archive, hap=self.write_hap(stale=True))

    def test_app_embedded_hap_uses_the_same_gate(self):
        hap = self.write_hap(stale=True)
        app = self.root / 'package.app'
        with ZipFile(app, 'w') as package:
            package.write(hap, 'entry.hap')
        with self.assertRaisesRegex(ValueError, 'stale Windows Steam resource'):
            offline.verify_archive(self.root, self.archive, app=app)

    def test_offline_mode_never_downloads_a_missing_archive(self):
        self.archive.unlink()
        with patch.object(offline.urllib.request, 'urlopen', side_effect=AssertionError('offline')):
            with self.assertRaisesRegex(ValueError, 'Missing pinned'):
                offline.prepare(self.root, self.root, offline=True)


if __name__ == '__main__':
    unittest.main()
