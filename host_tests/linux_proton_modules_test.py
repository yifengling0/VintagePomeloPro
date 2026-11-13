"""Original GE module closure must survive partial historical offline packages."""
import hashlib
import io
import tarfile
import tempfile
import zipfile
import json
from pathlib import Path
import sys
import unittest
import urllib.error
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'scripts'))
from prepare_linux_proton_module_closure import prepare, verify_contract, build, verify, RAW, LOCK, BASE, MEDIA
from proton_module_contract import DIRECTORIES, ASSEMBLIES, complete_module_lock

class ModuleSupportTests(unittest.TestCase):
    def test_complete_both_pe_widths_and_dynamic_modules(self):
        manifest=verify_contract()
        self.assertEqual(len(manifest['files']),1647)
        for name in ASSEMBLIES:
            row=next(r for r in manifest['files'] if r['original']==name)
            self.assertEqual(row['source'],'archive')
            self.assertTrue(row['path'].startswith('winsxs/'))
        files={r['path']:r for r in manifest['files']}
        for path in ('system32/concrt140.dll','system32/d3d11.dll','system32/dxgi.dll',
                     'system32/mfplat.dll','system32/winhttp.dll','system32/mfreadwrite.dll','syswow64/concrt140.dll',
                     'system32/icudt68.dll','system32/icuin68.dll','system32/icuuc68.dll',
                     'syswow64/icudt68.dll','syswow64/icuin68.dll','syswow64/icuuc68.dll'):
            self.assertIn(path,files)
        self.assertEqual(manifest['closure']['directories']['files/lib/wine/aarch64-windows']['files'],795)
        self.assertLess(manifest['archiveInstalledBytes'],manifest['installedBytes'])
        self.assertTrue(files['system32/winevulkan.dll']['builtin'])
        self.assertEqual(sum(bool(r.get('builtin')) for r in files.values()),21)
        self.assertEqual(manifest['builtinInstalledBytes'],21757952)
        self.assertTrue(all(not r['source'].startswith('media/') or r['original'].startswith('files/lib/wine/aarch64-windows/') for r in files.values()))

    def test_original_backend_and_private_driver_placement(self):
        m=verify_contract()
        self.assertTrue(all('/drivers/' in r['path'] for r in m['files'] if r['path'].endswith('.sys') and not r['path'].endswith('win32k.sys')))
        base=json.loads((ROOT/'docs/linux-steam/gnu-proton-probe.lock.json').read_text())
        self.assertIn(m['backend'],base['selected'])
        for r in m['files']:
            if r['source'].startswith('package/'):
                self.assertIn({k:r[k] for k in ('bytes','sha256')},[{k:v[k] for k in ('bytes','sha256')} for v in base['selected'] if v['path']==r['original']])

class CleanModuleRestoreTests(unittest.TestCase):
    def test_github_gateway_failure_uses_same_verified_release_asset(self):
        for invalid in ('none', 'metadata', 'checksum'):
            with self.subTest(invalid=invalid), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                data = b'verified source archive'
                source = 'https://github.com/GloriousEggroll/proton-ge-custom/releases/download/test/ge.tar.gz'
                dist = dict(file='ge.tar.gz', bytes=len(data), sha512=hashlib.sha512(data).hexdigest(), source=source)
                (root / LOCK).parent.mkdir(parents=True)
                (root / LOCK).write_text(json.dumps(dict(distribution=dist)))
                asset_url = 'https://api.github.com/repos/GloriousEggroll/proton-ge-custom/releases/assets/123'
                metadata = dict(tag_name='test', assets=[dict(name='ge.tar.gz', size=len(data),
                                                            browser_download_url=source, url=asset_url)])
                if invalid == 'metadata': metadata['assets'][0]['size'] += 1
                payload = data if invalid != 'checksum' else b'x' * len(data)
                replies = [urllib.error.HTTPError(source, 504, 'gateway', {}, None),
                           io.BytesIO(json.dumps(metadata).encode()), io.BytesIO(payload)]
                with patch('prepare_linux_proton_module_closure.urllib.request.urlopen', side_effect=replies) as download, \
                     patch('prepare_linux_proton_module_closure.build', return_value='built') as builder:
                    if invalid == 'none':
                        self.assertEqual(prepare(root), 'built')
                        self.assertEqual(download.call_args.args[0].full_url, asset_url)
                        self.assertEqual((root / 'build/linux-runtime-downloads/ge.tar.gz').read_bytes(), data)
                    else:
                        with self.assertRaises(ValueError): prepare(root)
                        builder.assert_not_called()
                        self.assertFalse((root / 'build/linux-runtime-downloads/ge.tar.gz').exists())
                    self.assertFalse((root / 'build/linux-runtime-downloads/ge.tar.part').exists())

    def test_clean_download_uses_pinned_source_and_checks_archive(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary)
            data=b'verified source archive'
            source='https://github.com/GloriousEggroll/proton-ge-custom/releases/download/test/ge.tar.gz'
            dist=dict(file='ge.tar.gz',bytes=len(data),sha512=hashlib.sha512(data).hexdigest(),source=source)
            (root/LOCK).parent.mkdir(parents=True)
            (root/LOCK).write_text(json.dumps(dict(distribution=dist)))
            with patch('prepare_linux_proton_module_closure.urllib.request.urlopen',return_value=io.BytesIO(data)) as download, patch('prepare_linux_proton_module_closure.build',return_value='built') as builder:
                self.assertEqual(prepare(root), 'built')
                self.assertEqual(download.call_args.args[0].full_url,source)
                cached=root/'build/linux-runtime-downloads/ge.tar.gz'
                self.assertEqual(cached.read_bytes(),data)
                builder.assert_called_once_with(root,cached)

    def test_clean_checkout_regenerates_and_rejects_corrupt_zip(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary)
            archive=root/'ge.tar.gz'
            names=[DIRECTORIES[0]+'/winevulkan.dll',DIRECTORIES[1]+'/concrt140.dll',DIRECTORIES[2]+'/winevulkan.so',DIRECTORIES[3]+'/icuuc68.dll',DIRECTORIES[4]+'/icuuc68.dll'] + list(ASSEMBLIES)
            with tarfile.open(archive,'w:gz') as tar:
                for name in names:
                    data=name.encode();info=tarfile.TarInfo('ge/'+name);info.size=len(data);tar.addfile(info,io.BytesIO(data))
            base=dict(distribution=dict(file=archive.name,bytes=archive.stat().st_size,sha512=hashlib.sha512(archive.read_bytes()).hexdigest()),selected=[dict(path=names[2],bytes=len(names[2].encode()),sha256=hashlib.sha256(names[2].encode()).hexdigest(),type='0',link='')])
            lock=complete_module_lock(base,archive)
            for path,value in [(BASE,base),(LOCK,lock),(MEDIA,dict(distribution=base['distribution'],files=[]))]:
                (root/path).parent.mkdir(parents=True,exist_ok=True);(root/path).write_text(json.dumps(value))
            first=build(root,archive)
            self.assertEqual(len(first['files']),8)
            self.assertTrue(all(r['source']=='archive' for r in first['files']))
            self.assertTrue(next(r for r in first['files'] if r['path']=='system32/winevulkan.dll')['builtin'])
            self.assertEqual(build(root,archive)['archive'],first['archive'],'same inputs reuse deterministic archive identity')
            # Even a container with a recomputed hash cannot omit/duplicate a module.
            target=root/RAW/'modules.zip'
            with zipfile.ZipFile(target,'w') as z:z.writestr('system32/concrt140.dll',b'corrupt')
            generated=json.loads((root/RAW/'manifest.json').read_text())
            generated['archive']=dict(bytes=target.stat().st_size,sha256=hashlib.sha256(target.read_bytes()).hexdigest())
            (root/RAW/'manifest.json').write_text(json.dumps(generated))
            with self.assertRaisesRegex(ValueError,'inventory'):verify(root)
            archive.write_bytes(b'wrong archive')
            with self.assertRaisesRegex(ValueError,'identity'):build(root,archive)

if __name__=='__main__':unittest.main()
