"""Release gating with real small ZIPs, including the HAP nested in an App."""
import io
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import prepare_release_offline_packages as release


class ReleaseOfflineTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.raw = self.root / release.RAW
        self.downloads = self.root / 'downloads'
        self.output = self.root / 'output'
        self.downloads.mkdir()
        (self.raw / 'linux-steam').mkdir(parents=True)
        (self.raw / 'linux-proton').mkdir()
        self.official = self.downloads / 'official.zip'
        self.official.write_bytes(b'official pinned archive')
        self.plan = dict(id='a' * 64, clientBuild='123', archives=[])
        self.set_official()
        self.set_proton()

    def set_proton(self, payload=b'upstream wine', adapter=b'adapter-v1'):
        upstream = {'package/proton': payload, 'package/files/bin/wine': b'wine executable'}
        # The bundled adapter ZIP and published import ZIP deliberately differ.
        runtime = self.downloads / 'gnu-proton.zip'
        self.write_zip(runtime, {**upstream, 'proton-steam-ready': b'published-sidecar'})
        full = self.raw / 'linux-proton/gnu-proton.zip'
        self.write_zip(full, {**upstream, 'proton-steam-ready': adapter})
        rows = [dict(path=n, bytes=len(data), sha256=hashlib.sha256(data).hexdigest(), mode=384)
                for n, data in upstream.items()]
        manifest = dict(bytes=full.stat().st_size, sha256=release.file_hash(full), files=rows)
        (full.parent / 'manifest.json').write_text(json.dumps(manifest))
        self.runtime = dict(id=hashlib.sha256(payload).hexdigest(), name='GE-Proton11-11 ARM64', files=rows,
                            archive=dict(url='https://github.com/yifengling0/VintagePomeloPro/releases/download/runtime/gnu-proton.zip',
                                         bytes=runtime.stat().st_size, sha256=release.file_hash(runtime)))
        self.write_catalog()

    @staticmethod
    def write_zip(path, files):
        with zipfile.ZipFile(path, 'w') as z:
            for name, data in files.items():
                z.writestr(zipfile.ZipInfo(name, (2026, 1, 1, 0, 0, 0)), data)

    def write_catalog(self):
        catalog = dict(schemaVersion=1, releases=[self.runtime])
        for path in [self.raw / 'linux-proton/runtime-catalog.json',
                     self.root / 'docs/linux-steam/proton-runtimes/catalog.json']:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(json.dumps(catalog))

    def set_official(self):
        self.plan['archives'] = [dict(id='official', url='https://client-update.steamstatic.com/official.zip',
                                     bytes=self.official.stat().st_size, sha256=release.file_hash(self.official))]
        (self.raw / 'linux-steam/deployment.json').write_text(json.dumps(self.plan))

    def prepare(self):
        with patch.object(release.urllib.request, 'urlopen', side_effect=AssertionError('offline must not download')):
            return release.prepare(self.root, self.downloads, self.output, allow_download=False)

    def hap(self):
        out = io.BytesIO()
        with zipfile.ZipFile(out, 'w') as z:
            for name in ['linux-steam/offline-package.json', 'linux-proton/runtime-catalog.json']:
                z.writestr('resources/rawfile/' + name, (self.raw / name).read_bytes())
        return out.getvalue()

    def test_prepare_reuses_unchanged_components_and_checks_hap_and_app(self):
        report = self.prepare()
        identities = [(self.output / item['fileName']).stat().st_mtime_ns for item in report['packages']]
        hap = self.root / 'test.hap'
        hap.write_bytes(self.hap())
        app = self.root / 'test.app'
        with zipfile.ZipFile(app, 'w', compression=zipfile.ZIP_DEFLATED) as z:
            z.writestr('entry.hap', hap.read_bytes())
        self.assertEqual(len(release.verify(self.root, self.output, hap, app)['checkedPackages']), 2)
        # A UI/host update must neither download nor rewrite either big ZIP.
        self.plan['id'] = 'b' * 64
        self.set_official()
        self.official.unlink()
        second = self.prepare()
        self.assertEqual(second['archiveSetId'], report['archiveSetId'])
        self.assertEqual([(self.output / p['fileName']).stat().st_mtime_ns for p in second['packages']], identities)

    def test_adapter_only_change_reuses_import_catalog_archive(self):
        first = self.prepare()
        tool = self.output / first['packages'][1]['fileName']
        self.assertEqual(release.file_hash(tool), self.runtime['archive']['sha256'])
        self.assertNotEqual(release.file_hash(tool), release.file_hash(self.raw / 'linux-proton/gnu-proton.zip'))
        mtime = tool.stat().st_mtime_ns
        self.set_proton(adapter=b'changed host, same upstream wine')
        (self.downloads / 'gnu-proton.zip').unlink()
        second = self.prepare()
        self.assertEqual(first['packages'][1], second['packages'][1])
        self.assertEqual(tool.stat().st_mtime_ns, mtime)

    def test_sha_named_runtime_cache_is_reused(self):
        source = self.downloads / 'gnu-proton.zip'
        source.rename(self.downloads / (self.runtime['archive']['sha256'] + '.zip'))
        self.prepare()

    def test_component_change_requires_new_bundle_and_rebuilt_hap(self):
        first = self.prepare()
        old = self.root / 'old.hap'
        old.write_bytes(self.hap())
        self.official.write_bytes(b'updated SDK archive')
        self.set_official()
        with self.assertRaisesRegex(ValueError, 'stale'):
            release.verify(self.root, self.output)
        second = self.prepare()
        self.assertNotEqual(first['archiveSetId'], second['archiveSetId'])
        self.assertNotEqual(first['packages'][0]['fileName'], second['packages'][0]['fileName'])
        with self.assertRaisesRegex(ValueError, 'different Steam'):
            release.verify(self.root, self.output, old)

    def test_corrupt_and_missing_publications_are_rejected(self):
        report = self.prepare()
        tool = self.output / report['packages'][1]['fileName']
        original = tool.read_bytes()
        tool.write_bytes(bytes(len(original)))
        with self.assertRaises(ValueError):
            release.verify(self.root, self.output)
        tool.write_bytes(original)
        (self.output / report['packages'][0]['fileName']).unlink()
        with self.assertRaisesRegex(ValueError, 'missing'):
            release.verify(self.root, self.output)

    def test_missing_cache_does_not_fallback_to_network(self):
        self.official.unlink()
        with self.assertRaisesRegex(ValueError, 'Missing cached'):
            self.prepare()
        self.assertFalse((self.output / release.REPORT).exists())

    def test_proton_change_has_distinct_name_and_rejects_old_hap(self):
        first = self.prepare()
        old = self.root / 'old.hap'
        old.write_bytes(self.hap())
        self.set_proton(payload=b'new upstream wine')
        second = self.prepare()
        self.assertNotEqual(first['packages'][1]['fileName'], second['packages'][1]['fileName'])
        with self.assertRaisesRegex(ValueError, 'Proton identity'):
            release.verify(self.root, self.output, old)

    def test_catalog_mismatch_and_internal_package_corruption_are_rejected(self):
        self.prepare()
        docs = self.root / 'docs/linux-steam/proton-runtimes/catalog.json'
        docs.write_text('{}')
        with self.assertRaisesRegex(ValueError, 'catalogs differ'):
            release.verify(self.root, self.output)
        self.write_catalog()
        manifest = self.raw / 'linux-proton/manifest.json'
        old = manifest.read_bytes()
        value = json.loads(old)
        value['files'][0]['sha256'] = '0' * 64
        manifest.write_text(json.dumps(value))
        with self.assertRaisesRegex(ValueError, 'bundled upstream'):
            release.verify(self.root, self.output)
        manifest.write_bytes(old)
        malformed = self.root / 'malformed.zip'
        for contents, message in [
            ({'package/proton': b'upstream wine'}, 'inventory'),
            ({'package/proton': b'broken___wine', 'package/files/bin/wine': b'wine executable'}, 'checksum'),
        ]:
            self.write_zip(malformed, contents)
            with self.assertRaisesRegex(ValueError, message):
                release.verify_proton_archive(malformed, self.runtime)

    def test_missing_runtime_cache_does_not_use_adapter_zip_or_network(self):
        (self.downloads / 'gnu-proton.zip').unlink()
        with self.assertRaisesRegex(ValueError, 'Missing cached Proton runtime'):
            self.prepare()

    def test_missing_runtime_downloads_pinned_catalog_bytes_and_reuses_cache(self):
        source = self.downloads / 'gnu-proton.zip'
        data = source.read_bytes()
        source.unlink()
        with patch.object(release.urllib.request, 'urlopen', return_value=io.BytesIO(data)) as request:
            report = release.prepare(self.root, self.downloads, self.output)
        request.assert_called_once_with(self.runtime['archive']['url'], timeout=45)
        tool = self.output / report['packages'][1]['fileName']
        self.assertEqual(tool.read_bytes(), data)
        tool.unlink()
        # The download's SHA-named cache, not the full adapter, repairs a
        # missing publication with network access disabled on the next run.
        self.assertEqual(self.prepare()['packages'][1], report['packages'][1])


if __name__ == '__main__':
    unittest.main()
