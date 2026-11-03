"""Pin both DXVK PE views, original GE bridge, native payload and source closure."""
import json
from pathlib import Path
import re
import subprocess
import sys
import unittest
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'scripts'))
from prepare_linux_proton_dxvk import verify, RAW, PROFILE, sources
from dxvk_pe import inspect_pe

class DxvkSupportTests(unittest.TestCase):
    def test_real_complete_companion(self):
        m=verify()
        self.assertEqual(len(m['files']),11)
        self.assertLess(m['installedBytes'],64*1048576)
        self.assertEqual(m['profile']['VN_WINEHUA_PERSISTENT_MAP_SYNC'],'0')
        self.assertEqual(m['profile']['VN_OHOS_PRECISE_SHADOW'],'1')

    def test_missing_each_width_and_bridge_fails_before_packaging(self):
        original=Path.stat
        for name in ('dlls/arm64x/d3d11.dll','dlls/i386/d3d11.dll','pe/vulkan-1.dll','libvulkan_virtio.so'):
            target=ROOT/RAW/name
            def missing(path,*args,**kwargs):
                if path==target:raise FileNotFoundError(str(path))
                return original(path,*args,**kwargs)
            with self.subTest(name=name),patch.object(Path,'stat',missing):
                with self.assertRaises(FileNotFoundError):verify()

    def test_hap_manifest_is_verified_with_payload(self):
        class WrongManifest:
            def read(self, path):
                self.path=path
                return b'{}'
        hap=WrongManifest()
        with self.assertRaisesRegex(ValueError,'HAP GNU DXVK manifest mismatch'):verify(hap=hap)
        self.assertEqual(hap.path,'resources/rawfile/linux-proton/dxvk-support/manifest.json')

    def test_old_binary_cannot_cover_new_source(self):
        changed=sources(ROOT)
        changed['scripts/build_linux_proton_venus.py']='0'*64
        with patch('prepare_linux_proton_dxvk.sources',return_value=changed):
            with self.assertRaisesRegex(ValueError,'source/profile/backend'):verify()

    def test_explicit_contract_is_local_to_selected_gnu_dxvk(self):
        # Prevent accidentally changing native coherent-map policy together
        # with this explicitly negotiated DXVK connection protocol.
        native=(ROOT/'entry/src/main/cpp/linux_steam/gnu_fixture/steamohos_fex.cpp').read_text()
        self.assertIn('VN_WINEHUA_PERSISTENT_MAP_SYNC',native)
        self.assertNotIn('VN_OHOS_PRECISE_SHADOW',native)
        launcher=(ROOT/'entry/src/main/cpp/linux_steam/proton_probe.cpp').read_text()
        for key,value in PROFILE.items():self.assertIn(key+'='+value,launcher)
        self.assertIn('d3d9=b;ddraw=b;wined3d=b',launcher)

    def test_gnu_elf_dependencies_have_runtime_members(self):
        deployment=json.loads((ROOT/'entry/src/main/resources/rawfile/linux-steam/deployment.json').read_text())
        runtime={Path(f['path']).name for f in deployment['files'] if f['path'].startswith('release/runtime/')}
        proton=json.loads((ROOT/'entry/src/main/resources/rawfile/linux-proton/manifest.json').read_text())
        runtime|={Path(f['path']).name for f in proton['files'] if f['path'].startswith(('graphics/','package/files/lib/'))}
        for name in ('libproton_probe.so','libproton_vulkan.so','libvulkan_virtio.so'):
            dynamic=subprocess.check_output(['readelf','-d',str(ROOT/RAW/name)],text=True)
            needed=set(re.findall(r'\(NEEDED\).*\[([^\]]+)\]',dynamic))
            self.assertIn('libc.so.6',needed,'GNU ABI required, not OHOS libc.so')
            self.assertFalse(needed-runtime,name+' has unshipped GNU dependencies: '+','.join(sorted(needed-runtime)))

    def test_pe_import_and_vulkan_forwarder_closure(self):
        upstream=json.loads((ROOT/'docs/linux-steam/gnu-proton-probe.lock.json').read_text())
        for arch in ('arm64x','i386'):
            directory='aarch64-windows' if arch=='arm64x' else 'i386-windows'
            available={Path(f['path']).name.lower() for f in upstream['selected'] if '/'+directory+'/' in f['path']}
            available|={'dxgi.dll','d3d11.dll','vulkan-1.dll','winevulkan.dll','d3dcompiler_47.dll'}
            for name in ('dxgi.dll','d3d11.dll'):
                graphs=inspect_pe((ROOT/RAW/'dlls'/arch/name).read_bytes())['graphs']
                for graph in graphs.values():
                    for dependency in graph['importModules']:
                        if re.fullmatch(r'api-ms-win-crt-[a-z0-9-]+\.dll',dependency):
                            self.assertIn('ucrtbase.dll',available)
                        else:self.assertIn(dependency,available,arch+'/'+name)
        vulkan=inspect_pe((ROOT/RAW/'pe/vulkan-1.dll').read_bytes())['graphs']
        wine=inspect_pe((ROOT/RAW/'pe/winevulkan.dll').read_bytes())['graphs']
        for view,graph in vulkan.items():
            exports={row['name'] for row in wine[view]['exports']}
            forwarded=0
            for row in graph['exports']:
                if 'forwarder' not in row:continue
                module,symbol=row['forwarder'].split('.',1)
                self.assertEqual(module.lower(),'winevulkan')
                self.assertIn(symbol,exports,view+' unresolved Vulkan forwarder')
                forwarded+=1
            self.assertGreater(forwarded,200)

if __name__=='__main__':unittest.main()
