"""Verify the official Linux client helper import boundary, using synthetic PE data."""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("client_helpers", ROOT / "scripts/import_linux_steam_client_helpers.py")
HELPERS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(HELPERS)


class ClientHelpers(unittest.TestCase):
    def test_only_pinned_helpers_are_extracted_and_manifest_is_reproducible(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            package = root / "official.zip"
            data = bytearray(128)
            data[:2] = b"MZ"
            data[60:64] = (64).to_bytes(4, "little")
            data[64:70] = b"PE\0\0\x64\x86"
            names = {"legacycompat/iscriptevaluator.exe": "install_script", "bin/d3ddriverquery64.exe": "driver_query"}
            with zipfile.ZipFile(package, "w") as source:
                for name in names:
                    source.writestr(name, data)
                source.writestr("legacycompat\\steamclient.dll", b"excluded")
                source.writestr("config/loginusers.vdf", b"never import account data")
            lock = {"packageSha1": hashlib.sha1(package.read_bytes()).hexdigest(), "package": "fake_linux_fixture",
                    "files": {name: {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(), "role": role}
                              for name, role in names.items()}}
            (root / "manifest.json").write_text(json.dumps({"files": {}, "steamClientProbe": {}}))
            first = HELPERS.import_helpers(package, root, lock)
            with zipfile.ZipFile(root / first["archive"]) as archive:
                self.assertEqual(set(archive.namelist()), set(names))
            self.assertEqual(first, HELPERS.import_helpers(package, root, lock))
            manifest = json.loads((root / "manifest.json").read_text())
            self.assertEqual(manifest["steamClientProbe"]["helpers"]["provenance"], lock)
            for changed in ["package", "helper", "allowlist"]:
                invalid = copy.deepcopy(lock)
                if changed == "package":
                    invalid["packageSha1"] = "0" * 40
                elif changed == "helper":
                    invalid["files"][next(iter(names))]["sha256"] = "0" * 64
                else:
                    invalid["files"]["../escape.exe"] = invalid["files"].pop(next(iter(names)))
                with self.assertRaises(ValueError):
                    HELPERS.import_helpers(package, root, invalid)


if __name__ == "__main__":
    unittest.main()
