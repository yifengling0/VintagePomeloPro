#!/usr/bin/env python3
"""Offline regressions for Gecko provenance, both architectures and extraction."""
import copy
import io
import json
from pathlib import Path
import struct
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import wine_gecko as gecko


def pe(machine):
    data = bytearray(128)
    data[:2] = b'MZ'
    struct.pack_into('<I', data, 60, 64)
    data[64:68] = b'PE\0\0'
    struct.pack_into('<H', data, 68, machine)
    return bytes(data)


class GeckoTest(unittest.TestCase):
    def setUp(self):
        self.lock = {'version': '2.47.4', 'archives': {}}
        self.payload = {}
        for arch, machine, slot in [('x86', 0x14c, 'i386-windows'),
                                     ('x86_64', 0x8664, 'aarch64-windows')]:
            directory = gecko.directory_name(self.lock, arch)
            data = {'VERSION': b'Wine Gecko 2.47.4', 'xul.dll': pe(machine)}
            self.lock['archives'][arch] = {'sha256': arch, 'files': {
                name: {'bytes': len(value), 'sha256': gecko.digest(value)} for name, value in data.items()}}
            self.payload.update({'share/wine/gecko/' + directory + '/' + name: value for name, value in data.items()})
            self.payload['bin/' + slot + '/mshtml.dll'] = directory.encode('utf-16le')
        self.manifest = gecko.component_manifest(self.lock)

    def validate(self):
        gecko.validate_payload(self.payload.keys(), self.payload.__getitem__, self.manifest, self.lock)

    def test_dual_arch_shared_directories(self):
        self.validate()

    def test_missing_architecture_is_rejected(self):
        self.payload = {n: b for n, b in self.payload.items() if '/wine-gecko-2.47.4-x86_64/' not in n}
        with self.assertRaisesRegex(ValueError, 'inventory'):
            self.validate()

    def test_corrupt_payload_is_rejected(self):
        self.payload['share/wine/gecko/wine-gecko-2.47.4-x86/xul.dll'] = b'corrupt'
        with self.assertRaisesRegex(ValueError, 'checksum'):
            self.validate()

    def test_wrong_pe_machine_even_with_matching_hash_is_rejected(self):
        item = self.lock['archives']['x86']['files']['xul.dll']
        item['sha256'] = gecko.digest(pe(0x8664))
        with self.assertRaisesRegex(ValueError, 'architecture'):
            gecko.check_file('xul.dll', pe(0x8664), item, 'x86', self.lock['version'])

    def test_wrong_version_even_with_matching_hash_is_rejected(self):
        data = b'Wine Gecko 2.47.3'
        item = {'bytes': len(data), 'sha256': gecko.digest(data)}
        with self.assertRaisesRegex(ValueError, 'VERSION'):
            gecko.check_file('VERSION', data, item, 'x86', self.lock['version'])

    def test_old_omission_manifest_is_rejected(self):
        self.manifest = {'status': 'not-bundled', 'reason': 'old profile'}
        with self.assertRaisesRegex(ValueError, 'manifest'):
            self.validate()

    def test_mshtml_architecture_version_pair_is_required(self):
        self.payload['bin/aarch64-windows/mshtml.dll'] = b'wrong version'
        with self.assertRaisesRegex(ValueError, 'mshtml'):
            self.validate()

    def test_unexpected_extra_payload_is_rejected(self):
        self.payload['share/wine/gecko/extra.msi'] = b'installer'
        with self.assertRaisesRegex(ValueError, 'inventory'):
            self.validate()

    def test_archive_paths(self):
        for name in ('../escape', '/absolute', 'a/../../escape', 'a\\escape', 'C:/escape', 'a//b'):
            with self.subTest(name=name), self.assertRaises(ValueError):
                gecko.safe_name(name)

    def test_rejects_archive_symlinks_before_writing(self):
        stream = io.BytesIO()
        with tarfile.open(fileobj=stream, mode='w') as tar:
            entry = tarfile.TarInfo('wine-gecko-2.47.4-x86/xul.dll')
            entry.type = tarfile.SYMTYPE
            entry.linkname = '/outside'
            tar.addfile(entry)
        stream.seek(0)
        with tempfile.TemporaryDirectory() as temp:
            archive = Path(temp) / 'fixture.tar'
            archive.write_bytes(stream.read())
            target = Path(temp) / 'out'
            with self.assertRaisesRegex(ValueError, 'archive member'):
                gecko.extract(archive, target, self.lock['archives']['x86'], 'x86', self.lock)
            self.assertFalse(target.exists())

    def test_wine_upgrade_requires_explicit_gecko_pair_update(self):
        with tempfile.TemporaryDirectory() as temp:
            for path in ('dlls/mshtml/nsiface.idl', 'dlls/appwiz.cpl/addons.c'):
                p = Path(temp) / path
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_text('#define GECKO_VERSION "2.47.4"')
            gecko.check_wine_version(temp, self.lock)
            p.write_text('#define GECKO_VERSION "2.99"')
            with self.assertRaisesRegex(ValueError, 'version mismatch'):
                gecko.check_wine_version(temp, self.lock)

    def test_offline_cache_reuse_does_not_change_stamp(self):
        with tempfile.TemporaryDirectory() as temp, patch.object(gecko, 'load_lock', return_value=self.lock):
            root = Path(temp)
            for name, data in self.payload.items():
                if not name.startswith('share/wine/gecko/'):
                    continue
                p = root / name.removeprefix('share/wine/gecko/')
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_bytes(data)
            gecko.prepare(root)
            stamp = (root / 'manifest.json').stat().st_mtime_ns
            with patch.object(gecko.urllib.request, 'urlopen', side_effect=AssertionError('network access')):
                gecko.prepare(root)
                gecko.prepare(root, check_only=True)
            self.assertEqual(stamp, (root / 'manifest.json').stat().st_mtime_ns)


if __name__ == '__main__':
    unittest.main()
