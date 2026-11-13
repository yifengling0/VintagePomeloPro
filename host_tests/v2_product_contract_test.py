import io
from pathlib import Path
import sys
import tempfile
import unittest
from zipfile import ZipFile
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from v2_product_contract import verify_members, verify_source_layout, verify_hap, REQUIRED_HOST, REQUIRED_GUEST

class BoundaryTests(unittest.TestCase):
    def test_shared_ohos_dependencies_remain_allowed(self):
        verify_members(['bin/aarch64-windows/libarm64ecfex.dll', 'libvulkan.so', 'dxvk/modern/x64/d3d11.dll'])
    def test_all_retired_payload_layers_are_rejected(self):
        for name in ['resources/rawfile/linux-steam/client.zip', 'resources/rawfile/linux-proton/gnu-proton.zip',
                     'libs/arm64-v8a/libsteamohos_gnu_child.so', 'bin/i386-windows/lsteamclient.dll',
                     'linux-steam-bridge.json', 'bin/hmosdeck-steam/steamclient.dll']:
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, 'Retired'):
                verify_members([name])
    def test_generated_cache_cannot_reenter_source_layout(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            verify_source_layout(root)
            p = root/'entry/src/main/resources/rawfile/linux-proton/cached.zip'
            p.parent.mkdir(parents=True); p.write_bytes(b'old cache')
            with self.assertRaisesRegex(ValueError, 'Retired'): verify_source_layout(root)
    def test_final_hap_checks_nested_runtime_and_required_components(self):
        with tempfile.TemporaryDirectory() as d:
            path=Path(d)/'test.hap'
            def write(extra=(), missing=None, registry=False, native=False):
                runtime=io.BytesIO()
                with ZipFile(runtime,'w') as wine:
                    for name in (*REQUIRED_GUEST, *extra):
                        if name != missing: wine.writestr(name, 'HMOSDECK_STEAM_BRIDGE'.encode('utf-16le') if registry and name.endswith('kernelbase.dll') else b'component')
                with ZipFile(path,'w') as hap:
                    for name in REQUIRED_HOST: hap.writestr('libs/arm64-v8a/'+name,b'probeLinuxSteamHost' if native and name == 'libentry.so' else b'component')
                    hap.writestr('resources/rawfile/wine-data.zip',runtime.getvalue())
            write(); self.assertTrue(verify_hap(path))
            write(['bin/i386-windows/lsteamclient.dll'])
            with self.assertRaisesRegex(ValueError,'Retired'): verify_hap(path)
            write(registry=True)
            with self.assertRaisesRegex(ValueError,'registry bridge'): verify_hap(path)
            write(native=True)
            with self.assertRaisesRegex(ValueError,'NAPI'): verify_hap(path)
            write(missing=REQUIRED_GUEST[0])
            with self.assertRaisesRegex(ValueError,'Missing'): verify_hap(path)

if __name__ == '__main__': unittest.main()
