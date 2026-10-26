"""The OHOS Wine Steam bridge must survive clean builds and reject mixed pairs."""
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import build_linux_steam_pe_components as bridge


def pe(machine, kernel=False):
    data = bytearray(2048); data[:2] = b'MZ'
    struct.pack_into('<I', data, 60, 128); data[128:132] = b'PE\0\0'
    struct.pack_into('<HH', data, 132, machine, 0)
    if kernel:
        data[64:80] = b'Wine builtin DLL'
        marker = 'HMOSDECK_STEAM_BRIDGE'.encode('utf-16le'); data[700:700 + len(marker)] = marker
    return bytes(data)


class BridgeTests(unittest.TestCase):
    def fixture(self):
        files = {name: pe(0x14c if '/i386-' in name else
                         0xaa64 if name.endswith('/kernelbase.dll') else 0x8664,
                         name.endswith('/kernelbase.dll')) for name in bridge.MODULES}
        value = {'schemaVersion': 1, 'source': 'self_built_linux_steam_bridge',
                 'files': [dict(path=n, bytes=len(d), sha256=bridge.sha(d)) for n, d in files.items()],
                 'gnuSha256': 'a' * 64, 'registryPatchSha256': bridge.sha((ROOT / bridge.PATCH).read_bytes())}
        files[bridge.MARKER] = json.dumps(value).encode()
        return files, value

    def test_complete_pair_and_missing_module(self):
        files, _ = self.fixture()
        bridge.validate_payload(files.__getitem__, 'a' * 64)
        del files['bin/i386-windows/lsteamclient.dll']
        with self.assertRaises(KeyError): bridge.validate_payload(files.__getitem__, 'a' * 64)

    def test_stale_gnu_patch_and_duplicate_inventory_are_rejected(self):
        files, original = self.fixture()
        variants = [dict(original, gnuSha256='b' * 64),
                    dict(original, registryPatchSha256='b' * 64),
                    dict(original, files=[original['files'][0]] * len(bridge.MODULES))]
        for value in variants:
            with self.subTest(value=value):
                files[bridge.MARKER] = json.dumps(value).encode()
                with self.assertRaisesRegex(ValueError, 'pairing is invalid'):
                    bridge.validate_payload(files.__getitem__, 'a' * 64)

    def test_wrong_abi_and_changed_pe_are_rejected(self):
        files, value = self.fixture(); name = 'bin/aarch64-windows/lsteamclient.dll'
        files[name] = pe(0xaa64)
        with self.assertRaisesRegex(ValueError, 'Invalid Linux Steam bridge module'):
            bridge.validate_payload(files.__getitem__, 'a' * 64)
        files, _ = self.fixture(); files[name] += b'changed'
        with self.assertRaisesRegex(ValueError, 'Unpaired'):
            bridge.validate_payload(files.__getitem__, 'a' * 64)

    def test_kernelbase_must_contain_the_session_patch(self):
        files, _ = self.fixture(); files['bin/i386-windows/kernelbase.dll'] = pe(0x14c)
        with self.assertRaisesRegex(ValueError, 'Kernelbase lacks'):
            bridge.validate_payload(files.__getitem__, 'a' * 64)

    def test_candidate_receipt_rejects_a_changed_bridge_even_with_updated_marker(self):
        files, value = self.fixture()
        receipt = {'payload': {n: dict(sha256=bridge.sha(d), bytes=len(d))
                               for n, d in files.items() if n in bridge.MODULES}}
        bridge.validate_payload(files.__getitem__, 'a' * 64, candidate=receipt)
        name = 'bin/aarch64-windows/lsteamclient.dll'; data = bytearray(files[name]); data[-1] = 1
        files[name] = bytes(data)
        for item in value['files']:
            if item['path'] == name: item['sha256'] = bridge.sha(data)
        files[bridge.MARKER] = json.dumps(value).encode()
        bridge.validate_payload(files.__getitem__, 'a' * 64)
        with self.assertRaisesRegex(ValueError, 'candidate payload changed'):
            bridge.validate_payload(files.__getitem__, 'a' * 64, candidate=receipt)

    def test_staging_cannot_replace_bridges_when_kernelbase_is_incompatible(self):
        files, _ = self.fixture()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); runtime = root / 'runtime'; output = root / 'output'
            for name, data in files.items():
                path = runtime / name; path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(data)
            for arch, name in bridge.BRIDGES.items():
                path = output / arch / 'lsteamclient.dll'; path.parent.mkdir(parents=True)
                path.write_bytes(files[name] + b'new build')
            kernel = runtime / 'bin/i386-windows/kernelbase.dll'; kernel.write_bytes(pe(0x14c))
            with patch.object(bridge, 'gnu_bridge', return_value='a' * 64):
                with self.assertRaisesRegex(ValueError, 'Kernelbase lacks'):
                    bridge.stage(runtime, output, root / 'rawfile')
                for name in bridge.BRIDGES.values(): self.assertEqual((runtime / name).read_bytes(), files[name])
                kernel.write_bytes(files['bin/i386-windows/kernelbase.dll'])
                bridge.stage(runtime, output, root / 'rawfile')
                bridge.validate_payload(lambda n: (runtime / n).read_bytes(), 'a' * 64)


if __name__ == '__main__': unittest.main()
