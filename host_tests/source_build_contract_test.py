"""Candidate provenance must not turn source rebuilds into accepted releases."""
import copy
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import source_build_contract as candidate
import runtime_core_contract as core


class CandidateTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        root = Path(cls.temp.name)
        source = root / 'test.c'
        source.write_text('int value(void) { return 42; }\n')
        binary = root / 'test.so'
        subprocess.run(['cc', '-shared', '-fPIC', '-g', str(source), '-o', str(binary)], check=True)
        # Synthetic fixture for format checks; never executed as ARM code.
        data = bytearray(binary.read_bytes())
        struct.pack_into('<H', data, 18, 183)
        cls.elf = bytes(data)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def receipt(self):
        return {'schemaVersion': 1, 'policy': candidate.POLICY, 'deviceValidated': False,
                'sources': {'checkout': {'commit': 'a' * 40}},
                'libraries': {'libs/arm64-v8a/test.so': {
                    'sha256': candidate.sha(self.elf), 'bytes': len(self.elf),
                    'loadableSha256': candidate.elf_identity(self.elf)}},
                'payload': {}, 'compilers': [{'sha256': 'b' * 64}]}

    def load(self, value, manifest=None):
        with patch.object(candidate, 'required_members', return_value=({'libs/arm64-v8a/test.so'}, set())):
            return candidate.load(lambda _: json.dumps(value).encode(), manifest or {'commit': 'a' * 40})

    def test_manifest_and_device_acceptance_are_mandatory(self):
        self.load(self.receipt())
        for key, value in (('deviceValidated', True), ('policy', 'accepted-release'), ('compilers', [])):
            record = self.receipt(); record[key] = value
            with self.assertRaises(ValueError): self.load(record)
        with self.assertRaises(ValueError): self.load(self.receipt(), {'commit': 'c' * 40})

    def test_missing_or_extra_inventory_is_rejected(self):
        record = self.receipt(); record['libraries'] = {}
        with self.assertRaises(ValueError): self.load(record)

    def test_elf_strip_preserves_identity_but_code_changes_do_not(self):
        record = self.receipt(); name = 'libs/arm64-v8a/test.so'
        candidate.validate_member(record, 'libraries', name, self.elf)
        # llvm-strip is unnecessary: alter only the non-loadable .comment bytes.
        changed = self.elf.replace(b'GCC:', b'XYZ:', 1)
        self.assertNotEqual(changed, self.elf)
        candidate.validate_member(record, 'libraries', name, changed)
        code = bytearray(self.elf)
        # e_flags belongs to the loadable identity, unlike compiler comments.
        code[48] ^= 1
        with self.assertRaises(ValueError): candidate.validate_member(record, 'libraries', name, bytes(code))

    def test_wrong_elf_architecture_is_rejected(self):
        data = bytearray(self.elf); struct.pack_into('<H', data, 18, 62)
        with self.assertRaises(ValueError): candidate.validate_binary('libs/arm64-v8a/test.so', bytes(data))

    def test_default_release_lock_remains_strict(self):
        record = self.receipt(); name = 'libs/arm64-v8a/test.so'
        with tempfile.TemporaryDirectory() as directory:
            lock = Path(directory) / 'lock.json'
            lock.write_text(json.dumps({'schemaVersion': 1, 'libraries': {name: '0' * 64}}))
            with patch.object(core, 'LOCK', lock):
                with self.assertRaisesRegex(ValueError, 'Unreviewed'):
                    core.validate_core(lambda _: self.elf)
                core.validate_core(lambda _: self.elf, candidate=record)

    def test_payload_checksum_and_pe_abi_both_required(self):
        data = bytearray(512); data[:2] = b'MZ'
        struct.pack_into('<I', data, 60, 128); data[128:132] = b'PE\0\0'
        struct.pack_into('<HH', data, 132, 0x14c, 0)
        name = 'bin/i386-windows/test.dll'
        record = {'payload': {name: {'sha256': candidate.sha(data), 'bytes': len(data)}}}
        candidate.validate_member(record, 'payload', name, bytes(data))
        data[-1] = 1
        with self.assertRaises(ValueError): candidate.validate_member(record, 'payload', name, bytes(data))
        with self.assertRaises(ValueError): candidate.validate_binary('dxvk/modern-2.6/arm64x/test.dll', bytes(data))


if __name__ == '__main__':
    unittest.main()
