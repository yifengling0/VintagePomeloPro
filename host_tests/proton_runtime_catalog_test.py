"""Upstream identity, package-only publishing and repeatable archive validation."""
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('runtime_catalog', ROOT / 'scripts/build_proton_runtime_catalog.py')
catalog = importlib.util.module_from_spec(spec)
spec.loader.exec_module(catalog)

class CatalogTests(unittest.TestCase):
    def test_upstream_identity_is_independent_of_adapter_and_archive(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            raw = root / 'entry/src/main/resources/rawfile/linux-proton'
            raw.mkdir(parents=True)
            docs = root / 'docs/linux-steam'
            docs.mkdir(parents=True)
            content = {'package/proton': b'upstream', 'package/files/lib/libx.so.1.2': b'library',
                       'package/files/lib/libx.so.1': b'library', 'graphics/adapter.so': b'adapter'}
            source = raw / 'gnu-proton.zip'
            with zipfile.ZipFile(source, 'w') as archive:
                for name, data in content.items(): archive.writestr(name, data)
            rows = [dict(path=name, bytes=len(data), sha256=hashlib.sha256(data).hexdigest(), mode=384)
                    for name, data in content.items()]
            descriptor = dict(distribution=dict(tag='test'), files=rows, installedBytes=sum(row['bytes'] for row in rows),
                              bytes=source.stat().st_size, sha256=catalog.sha(source))
            lock = dict(components=[dict(path='linux-proton/gnu-proton.zip', url='https://example.invalid/test.zip',
                        bytes=descriptor['bytes'], sha256=descriptor['sha256'])])
            (docs / 'build-components.lock.json').write_text(json.dumps(lock))
            (docs / 'gnu-proton-probe.lock.json').write_text(json.dumps(dict(aliases={'files/lib/libx.so.1.2':'libx.so.1'})))
            (raw / 'manifest.json').write_text(json.dumps(descriptor))
            release = catalog.release(root)
            self.assertEqual(len(release['files']), 3)
            alias = next(row for row in release['files'] if row['path'].endswith('/libx.so.1'))
            self.assertEqual(alias['sourcePath'], 'package/files/lib/libx.so.1.2')
            descriptor['files'][-1]['sha256'] = 'f' * 64
            (raw / 'manifest.json').write_text(json.dumps(descriptor))
            self.assertEqual(catalog.release(root)['id'], release['id'])
            target = root / 'upstream.zip'
            package = catalog.split_archive(source, target, release)
            before = target.stat().st_mtime_ns
            self.assertEqual(catalog.split_archive(source, target, release), package)
            self.assertEqual(target.stat().st_mtime_ns, before, 'reuse an identical archive')
            with zipfile.ZipFile(target) as archive:
                self.assertEqual(set(archive.namelist()), set(content) - {'graphics/adapter.so'})
                self.assertEqual(archive.read('package/proton'), content['package/proton'])
            source.write_bytes(b'damaged')
            with self.assertRaisesRegex(ValueError, 'differs'): catalog.split_archive(source, target, release)
            lock['components'][0]['sha256'] = 'f' * 64
            (docs / 'build-components.lock.json').write_text(json.dumps(lock))
            with self.assertRaisesRegex(ValueError, 'lock differ'): catalog.release(root)

if __name__ == '__main__': unittest.main()
