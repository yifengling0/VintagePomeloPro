"""The clean assembly producer must satisfy the real graphics validators."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import dxvk_runtime_contract
from package_texture_compat_runtime import validate


def sha(data):
    return hashlib.sha256(data).hexdigest()


class AssemblyManifestTests(unittest.TestCase):
    def test_real_clean_assembly_metadata_covers_every_packaged_dll(self):
        lock = json.loads((ROOT / 'docs/graphics/texture-compat-runtime.lock.json').read_text())
        script = (ROOT / 'scripts/assemble.sh').read_text()
        producer = script.split("<<'PYDXVK'\n", 1)[1].split('\nPYDXVK', 1)[0]
        files = {}
        seed = {'schemaVersion': 2, 'venusRuntime': {}, 'runtimes': {}}
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            manifest_path = root / 'dxvk/manifest.json'
            manifest_path.parent.mkdir(parents=True)
            for profile, record in lock['runtimes'].items():
                seed['runtimes'][profile] = {'commit': record['commit'], 'version': profile}
                for name in record['files']:
                    member = 'dxvk/' + profile + '/' + name
                    data = member.encode() + b' FP32 DenormFlushToZero disabled'
                    files[member] = data
                    path = root / member
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_bytes(data)
                    record['files'][name] = sha(data)
                    architecture, dll = name.split('/')
                    # This is what the production heredoc emits before its
                    # filesystem inventory pass: only D3D11 and DXGI entries.
                    if dll in ('d3d11.dll', 'dxgi.dll'):
                        seed['runtimes'][profile].setdefault(architecture, {})[dll] = sha(data)
            icd = b'fixture ICD'
            lock['venus']['sha256'] = sha(icd)
            files['bin/guest_vulkan/lib/libvulkan_virtio.so'] = icd
            files['bin/guest_vulkan/manifest.json'] = json.dumps({
                'files': {'lib/libvulkan_virtio.so': sha(icd)},
                'guestMesaCommit': lock['venus']['commit']}).encode()
            seed['venusRuntime']['guestIcdSha256'] = sha(icd)
            manifest_path.write_text(json.dumps(seed))
            class Runtime:
                def read(self, name):
                    if name == 'dxvk/manifest.json':
                        return manifest_path.read_bytes()
                    return files[name]
            runtime = Runtime()
            with patch.object(dxvk_runtime_contract, 'load_modern_lock',
                              return_value=lock['runtimes']['modern-2.6']):
                with self.assertRaisesRegex(KeyError, 'd3d10.dll'):
                    validate(runtime, icd, lock)
                subprocess.run([sys.executable, '-', str(manifest_path)], input=producer,
                               text=True, check=True, capture_output=True)
                validate(runtime, icd, lock)
                generated = json.loads(manifest_path.read_text())
                self.assertEqual(generated['venusRuntime'], seed['venusRuntime'])
                for profile, record in lock['runtimes'].items():
                    for name, expected in record['files'].items():
                        architecture, dll = name.split('/')
                        self.assertEqual(generated['runtimes'][profile][architecture][dll], expected)
                # Changes after inventory generation still fail the normal gate.
                files['dxvk/legacy/x86/d3d10.dll'] += b'changed'
                with self.assertRaisesRegex(ValueError, 'stale DXVK'):
                    validate(runtime, icd, lock)


if __name__ == '__main__':
    unittest.main()
