import io
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
from zipfile import ZipFile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from normalize_unsigned_hap import normalize_hap


class UnsignedHapManifestOrderTest(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.hap = Path(directory.name) / "entry.hap"

    def write_hap(self, manifest="module.json"):
        with ZipFile(self.hap, "w") as archive:
            archive.writestr("resources/rawfile/coffee_config.json", "{}")
            archive.writestr("resources/rawfile/value_added_config.json", "{}")
            archive.writestr(manifest, '{"app":{"bundleName":"com.vintage.pomelopro"}}')
            archive.writestr("ets/modules.abc", b"payload")
            archive.comment = b"archive comment"

    def assert_compatible(self, manifest="module.json"):
        original = self.hap.read_bytes()
        with ZipFile(io.BytesIO(original)) as before:
            self.assertRaisesRegex(ValueError, "precedes", normalize_hap, self.hap, check_only=True)
            self.assertTrue(normalize_hap(self.hap))
            with ZipFile(self.hap) as after:
                self.assertEqual(manifest, after.namelist()[0])
                self.assertEqual(before.start_dir, after.start_dir)
                self.assertEqual(before.comment, after.comment)
                self.assertEqual({name: before.read(name) for name in before.namelist()},
                                 {name: after.read(name) for name in after.namelist()})
                offset = after.start_dir
        fixed = self.hap.read_bytes()
        self.assertEqual(original[:offset], fixed[:offset])
        self.assertFalse(normalize_hap(self.hap))
        self.assertFalse(normalize_hap(self.hap, check_only=True))
        self.assertEqual(fixed, self.hap.read_bytes())

    def test_reorders_stage_manifest_without_rewriting_local_records(self):
        self.write_hap()
        self.assert_compatible()

    def test_handles_zip64_index_and_legacy_manifest(self):
        with patch("zipfile.ZIP64_LIMIT", 128):
            self.write_hap("config.json")
        self.assert_compatible("config.json")

    def test_rejects_signed_hap(self):
        self.write_hap()
        with ZipFile(self.hap) as archive:
            offset = archive.start_dir
        original = self.hap.read_bytes()
        eocd = original.rindex(b"PK\x05\x06")
        signing_footer = b"\0" * 12 + b"<hap sign block>\x03\0\0\0"
        signed = bytearray(original[:offset] + signing_footer + original[offset:])
        struct.pack_into("<I", signed, eocd + len(signing_footer) + 16, offset + len(signing_footer))
        self.hap.write_bytes(signed)
        self.assertRaisesRegex(ValueError, "before signing", normalize_hap, self.hap)
        self.assertEqual(signed, self.hap.read_bytes())


if __name__ == "__main__":
    unittest.main()
