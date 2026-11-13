"""Reapply the real Box overlay to pinned source files and reject stale binaries."""
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('box_contract', ROOT/'scripts/apply_box64_wow64_patches.py')
box = importlib.util.module_from_spec(spec)
spec.loader.exec_module(box)


class BoxOverlayTest(unittest.TestCase):
    def test_clean_source_apply_idempotence_and_drift(self):
        lock = box.contract()
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory)
            for name in lock['files']:
                path = source/name
                path.parent.mkdir(parents=True, exist_ok=True)
                data = subprocess.check_output(['git', '-C', str(ROOT/'thirdparty/box64'),
                                                'show', lock['sourceCommit']+':'+name])
                path.write_bytes(data)
            with self.assertRaises(ValueError):
                box.apply(source, verify_only=True)
            box.apply(source)
            box.apply(source, verify_only=True)
            box.apply(source)
            path.write_bytes(path.read_bytes()+b'\n/* unexpected source drift */\n')
            with self.assertRaises(ValueError):
                box.apply(source)

    def test_stale_and_wrong_arch_modules_are_rejected(self):
        data=bytearray(256)
        data[:2]=b'MZ'
        struct.pack_into('<I',data,60,64)
        data[64:68]=b'PE\0\0'
        struct.pack_into('<H',data,68,0xaa64)
        with self.assertRaises(ValueError):
            box.validate_module(data)
        data.extend(box.contract()['runtimeMarker'].encode())
        box.validate_module(data)
        struct.pack_into('<H',data,68,0x8664)
        with self.assertRaises(ValueError):
            box.validate_module(data)


if __name__ == '__main__':
    unittest.main()
