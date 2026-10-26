#!/usr/bin/env python3
"""Exercise clean checkout restore, source drift and final HAP rejection."""
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
import zipfile

REPO=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('components',REPO/'scripts/prepare_linux_runtime_components.py')
components=importlib.util.module_from_spec(spec);spec.loader.exec_module(components)


class RestoreTests(unittest.TestCase):
    def setUp(self):
        self.temporary=tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root=Path(self.temporary.name)
        self.raw=self.root/'entry/src/main/resources/rawfile'
        self.cache=self.root/'build/cache'
        self.source=self.root/'adapter.c'; self.source.write_text('source v1')
        self.downloads={}; self.calls=[]; self.lock=dict(schemaVersion=1,
            sourceInputs={'adapter.c':components.sha(self.source)},components=[])
        for relative,manifest in components.DESTINATIONS.items():
            member='adapter.so' if relative.startswith('linux-proton/') else 'release/runtime/adapter.so'
            data=(relative+' compiled').encode(); stream=io.BytesIO()
            with zipfile.ZipFile(stream,'w') as z: z.writestr(member,data)
            blob=stream.getvalue(); checksum=hashlib.sha256(blob).hexdigest()
            metadata=dict(path=member,bytes=len(data),sha256=hashlib.sha256(data).hexdigest())
            value=dict(bytes=len(blob),sha256=checksum,files=[metadata])
            if relative.startswith('linux-steam/'):
                value=dict(archives=[dict(id='gnu-runtime',bytes=len(blob),sha256=checksum)],
                    files=[dict(**metadata,archive='gnu-runtime',zipPath=member)])
            target=self.raw/manifest;target.parent.mkdir(parents=True,exist_ok=True)
            target.write_text(json.dumps(value))
            url='https://github.com/yifengling0/VintagePomeloPro/releases/download/build-deps-gnu-test/'+relative.split('/')[-1]
            self.lock['components'].append(dict(path=relative,bytes=len(blob),sha256=checksum,
                manifestSha256=components.sha(target),url=url))
            self.downloads[url]=blob
        (self.root/'docs/linux-steam').mkdir(parents=True)
        self.write_lock()

    def write_lock(self):
        (self.root/'docs/linux-steam/build-components.lock.json').write_text(json.dumps(self.lock))

    def opener(self,request,timeout):
        self.calls.append(request.full_url)
        self.assertLessEqual(timeout,90)
        return io.BytesIO(self.downloads[request.full_url])

    def restore(self,verify_only=False):
        return components.prepare(self.root,self.lock,self.cache,verify_only,self.opener)

    def test_clean_checkout_and_offline_reuse(self):
        self.assertEqual(self.restore()['status'],'PASS')
        self.assertEqual(len(self.calls),2)
        self.cache.rename(self.root/'unavailable-cache')
        self.assertEqual(self.restore(True)['status'],'PASS')
        self.assertEqual(len(self.calls),2,'verified-only check must not download')

    def test_source_drift_fails_before_network(self):
        self.source.write_text('new source without rebuilt component')
        with self.assertRaisesRegex(ValueError,'source changed'):self.restore()
        self.assertEqual(self.calls,[])

    def test_missing_payload_not_silently_omitted(self):
        with self.assertRaisesRegex(ValueError,'Missing or corrupt'):self.restore(True)
        self.assertEqual(self.calls,[])

    def test_corrupt_rawfile_repaired_from_verified_cache(self):
        self.restore();target=self.raw/self.lock['components'][0]['path']
        target.write_bytes(b'damaged')
        with self.assertRaisesRegex(ValueError,'Missing or corrupt'):self.restore(True)
        self.restore();self.assertEqual(len(self.calls),2)

    def test_bad_download_does_not_replace_existing_file(self):
        row=self.lock['components'][0];target=self.raw/row['path'];target.write_bytes(b'keep')
        self.downloads[row['url']]=b'x'*row['bytes']
        with self.assertRaisesRegex(ValueError,'identity mismatch'):self.restore()
        self.assertEqual(target.read_bytes(),b'keep')

    def test_oversized_download_bounded(self):
        row=self.lock['components'][0];self.downloads[row['url']]=b'x'*(row['bytes']+1)
        with self.assertRaisesRegex(ValueError,'Oversized'):self.restore()
        self.assertFalse((self.raw/row['path']).exists())

    def test_manifest_drift_not_accepted(self):
        target=self.raw/components.DESTINATIONS[self.lock['components'][0]['path']]
        target.write_text('{}')
        with self.assertRaisesRegex(ValueError,'manifest changed'):self.restore()
        self.assertEqual(self.calls,[])

    def test_final_hap_checks_both_components(self):
        self.restore();self.write_lock();hap=self.root/'valid.hap'
        with zipfile.ZipFile(hap,'w') as z:
            for p in self.raw.rglob('*'):
                if p.is_file():z.write(p,'resources/rawfile/'+p.relative_to(self.raw).as_posix())
        self.assertEqual(components.verify_hap(hap,self.root)['components'],2)
        with zipfile.ZipFile(self.root/'bad.hap','w') as z:
            z.writestr('resources/rawfile/linux-proton/gnu-proton.zip',b'corrupt')
        with self.assertRaises((ValueError,KeyError)):components.verify_hap(self.root/'bad.hap',self.root)

    def test_incomplete_lock_rejected(self):
        self.lock['components'].pop()
        with self.assertRaisesRegex(ValueError,'Both GNU'):self.restore()


if __name__=='__main__':unittest.main()
