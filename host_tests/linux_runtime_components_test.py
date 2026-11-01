#!/usr/bin/env python3
"""Exercise clean checkout restore, source drift and final HAP rejection."""
import hashlib
import importlib.util
import io
import json
import struct
import sys
from pathlib import Path
import tempfile
import unittest
import zipfile

REPO=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(REPO/'scripts'))
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
        self.core=self.raw/'linux-steam/bootstrap/native-guest-core-x86_64'
        self.core.mkdir(parents=True)
        # Small ELF identity fixtures; these tests validate packaging, not execution.
        data=b'\x7fELF\x02\x01'+bytes(12)+b'\x3e\x00'+bytes(44)
        for name in components.native_guest_core.ADAPTER_SOURCES:
            source=self.root/name;source.parent.mkdir(parents=True,exist_ok=True);source.write_bytes((REPO/name).read_bytes())
        for name in components.native_guest_core.NAMES:
            (self.core/name).write_bytes(data)
        (self.core/'manifest.json').write_text(json.dumps(dict(schemaVersion=1,
            architecture='x86_64-linux-gnu',adapterSources=components.native_guest_core.adapter_sources(self.root),packages=[dict(source=s,path=p,sha256=h)
                for s,p,h in components.native_guest_core.PACKAGES],
            files={name:dict(bytes=len(data),sha256=hashlib.sha256(data).hexdigest())
                for name in components.native_guest_core.NAMES})))
        self.downloads={}; self.calls=[]; self.lock=dict(schemaVersion=1,
            sourceInputs={'adapter.c':components.sha(self.source)},components=[])
        for relative,manifest in components.DESTINATIONS.items():
            member='adapter.so' if relative.startswith('linux-proton/') else 'release/runtime/adapter.so'
            data=(relative+' compiled').encode(); stream=io.BytesIO()
            contents={member:data}
            if relative.startswith('linux-proton/'):
                contents.update({'graphics/'+name:b'verified driver fixture' for name in components.native_vulkan_driver.NAMES})
            with zipfile.ZipFile(stream,'w') as z:
                for name,payload in contents.items():z.writestr(name,payload)
            blob=stream.getvalue(); checksum=hashlib.sha256(blob).hexdigest()
            metadata=dict(path=member,bytes=len(data),sha256=hashlib.sha256(data).hexdigest())
            value=dict(bytes=len(blob),sha256=checksum,files=[dict(path=name,bytes=len(payload),sha256=hashlib.sha256(payload).hexdigest()) for name,payload in contents.items()])
            if relative.startswith('linux-steam/'):
                value=dict(archives=[dict(id='gnu-runtime',bytes=len(blob),sha256=checksum)],
                    files=[dict(**metadata,archive='gnu-runtime',zipPath=member)])
            target=self.raw/manifest;target.parent.mkdir(parents=True,exist_ok=True)
            target.write_text(json.dumps(value))
            url='https://github.com/yifengling0/VintagePomeloPro/releases/download/build-deps-gnu-test/'+relative.split('/')[-1]
            self.lock['components'].append(dict(path=relative,bytes=len(blob),sha256=checksum,
                manifestSha256=components.sha(target),url=url))
            self.downloads[url]=blob
        driver=self.raw/components.native_vulkan_driver.RELATIVE
        driver.parent.mkdir(parents=True,exist_ok=True)
        driver.write_text(json.dumps(components.native_vulkan_driver.expected(self.root)))
        (self.root/'docs/linux-steam').mkdir(parents=True)
        # verify_sources also validates the independently delivered Proton
        # bootstrap. Keep the packaging fixture complete with small PE64
        # identities, rather than depending on a developer's full runtime.
        import build_linux_proton_bootstrap as bootstrap
        pe=bytearray(512);pe[:2]=b'MZ';struct.pack_into('<I',pe,60,64)
        pe[64:68]=b'PE\0\0';struct.pack_into('<H',pe,68,0xaa64)
        struct.pack_into('<H',pe,84,240);struct.pack_into('<H',pe,88,0x20b)
        folder=self.raw/'linux-proton/bootstrap';folder.mkdir(parents=True,exist_ok=True)
        files=[]
        for name in bootstrap.NAMES:
            (folder/name).write_bytes(pe)
            files.append(dict(name=name,sourcePath='files/lib/wine/aarch64-windows/'+name,
                bytes=len(pe),sha256=hashlib.sha256(pe).hexdigest()))
        (folder/'manifest.json').write_text(json.dumps(dict(schemaVersion=1,
            architecture='aarch64-windows',distributionSha512='fixture',files=files)))
        (self.root/'docs/linux-steam/gnu-proton-probe.lock.json').write_text(json.dumps(
            dict(distribution=dict(sha512='fixture'),selected=[])))
        (self.root/'docs/linux-steam/proton-media.lock.json').write_text(json.dumps(dict(files=[])))
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

    def test_delivery_only_changes_keep_binary_source_gate(self):
        for name in components.DELIVERY_INPUTS:
            target=self.root/name;target.parent.mkdir(parents=True,exist_ok=True)
            target.write_bytes((REPO/name).read_bytes())
        target=self.raw/'linux-steam/deployment.json'
        target.write_bytes((REPO/'entry/src/main/resources/rawfile/linux-steam/deployment.json').read_bytes())
        offline=self.raw/'linux-steam/offline-package.json'
        original_offline=(REPO/'entry/src/main/resources/rawfile/linux-steam/offline-package.json').read_bytes()
        offline.write_bytes(original_offline)
        self.lock['deliveryInputs']={name:components.sha(self.root/name) for name in components.DELIVERY_INPUTS}
        components.verify_sources(self.root,self.lock)
        offline.write_text('{"archiveSetId":"old-package-missing-sdk"}')
        with self.assertRaisesRegex(ValueError,'offline package'):components.verify_sources(self.root,self.lock)
        offline.write_bytes(original_offline)
        self.source.write_text('native change cannot be reclassified as delivery')
        with self.assertRaisesRegex(ValueError,'source changed'):components.verify_sources(self.root,self.lock)
        self.source.write_text('source v1')
        self.lock['deliveryInputs']['adapter.c']=components.sha(self.source)
        with self.assertRaisesRegex(ValueError,'Unapproved GNU delivery'):components.verify_sources(self.root,self.lock)
        del self.lock['deliveryInputs']['adapter.c']
        (self.root/'scripts/linux_steam_native_sdk.py').write_text('unexpected delivery change')
        with self.assertRaisesRegex(ValueError,'delivery input changed'):components.verify_sources(self.root,self.lock)

    def test_missing_and_corrupt_guest_core_fail_before_network(self):
        target=self.core/'libc.so.6';data=target.read_bytes();target.unlink()
        with self.assertRaises(FileNotFoundError):self.restore()
        target.write_bytes(data[:-1]+b'x')
        with self.assertRaisesRegex(ValueError,'Guest core mismatch'):self.restore()
        self.assertEqual(self.calls,[])

    def test_final_hap_requires_guest_core(self):
        self.restore();self.write_lock()
        hap=self.root/'missing-core.hap'
        with zipfile.ZipFile(hap,'w') as z:
            for path in self.raw.rglob('*'):
                if path.is_file() and path.name!='libc.so.6':
                    z.write(path,'resources/rawfile/'+path.relative_to(self.raw).as_posix())
        with self.assertRaises(KeyError):components.verify_hap(hap,self.root)

    def test_guest_ipc_required_in_restore_hap_and_device_installer(self):
        name='libsteamohos_native_ipc.so'
        self.assertIn(name,components.native_guest_core.NAMES)
        installer=(REPO/'entry/src/main/ets/service/LinuxSteamClientSupervisor.ets').read_text()
        self.assertIn("'"+name+"'",installer)
        self.restore();self.write_lock()
        target=self.core/name;payload=target.read_bytes();target.unlink()
        with self.assertRaises(FileNotFoundError):self.restore(True)
        target.write_bytes(payload)
        hap=self.root/'missing-ipc.hap'
        with zipfile.ZipFile(hap,'w') as z:
            for path in self.raw.rglob('*'):
                if path.is_file() and path.name!=name:
                    z.write(path,'resources/rawfile/'+path.relative_to(self.raw).as_posix())
        with self.assertRaises(KeyError):components.verify_hap(hap,self.root)

    def test_driver_contract_must_match_original_proton_package(self):
        path=self.raw/components.native_vulkan_driver.RELATIVE
        value=json.loads(path.read_text());value['files']['libvulkan_virtio.so']['sha256']='0'*64
        path.write_text(json.dumps(value))
        with self.assertRaisesRegex(ValueError,'Native Vulkan driver manifest mismatch'):self.restore()
        self.assertEqual(self.calls,[])

    def test_explicit_hap_source_override_requires_matching_source_and_binary(self):
        self.source.write_text('new source with an independently delivered HAP companion')
        folder=self.raw/'linux-steam/bootstrap';folder.mkdir(parents=True,exist_ok=True)
        binary=folder/'libsteamohos_posix_hap.so';binary.write_bytes(b'independent-companion')
        manifest=folder/'posix-manifest.json'
        data=dict(schemaVersion=1,architecture='aarch64-linux-gnu',name=binary.name,
            bytes=binary.stat().st_size,sha256=components.sha(binary),sources={'adapter.c':components.sha(self.source)})
        manifest.write_text(json.dumps(data))
        self.lock['hapSourceOverrides']={'adapter.c':str(manifest.relative_to(self.root))}
        self.assertEqual(self.restore()['status'],'PASS')
        self.source.write_text('unbuilt change')
        with self.assertRaisesRegex(ValueError,'override source changed'):self.restore(True)
        self.source.write_text('new source with an independently delivered HAP companion')
        binary.write_bytes(b'corrupted')
        with self.assertRaisesRegex(ValueError,'override binary mismatch'):self.restore(True)

    def test_missing_payload_not_silently_omitted(self):
        with self.assertRaisesRegex(ValueError,'Missing or corrupt'):self.restore(True)
        self.assertEqual(self.calls,[])

    def add_native_companions(self):
        self.lock['hapCompanions'] = list(components.HAP_COMPANIONS)
        for relative, name in components.HAP_COMPANIONS.items():
            manifest = self.root / relative
            manifest.parent.mkdir(parents=True, exist_ok=True)
            binary = manifest.parent / name
            binary.write_bytes(('compiled ' + name).encode())
            manifest.write_text(json.dumps(dict(schemaVersion=1, architecture='aarch64-linux-gnu',
                name=name, bytes=binary.stat().st_size, sha256=components.sha(binary),
                sources={'adapter.c': components.sha(self.source)})))

    def test_native_companions_checked_even_without_source_overrides(self):
        self.add_native_companions()
        self.assertEqual(self.restore()['status'], 'PASS')
        (self.raw/'linux-steam/bootstrap/steamohos_fex').write_bytes(b'corrupt')
        with self.assertRaisesRegex(ValueError, 'override binary mismatch'): self.restore(True)

    def test_unapproved_companion_rejected_before_network(self):
        self.lock['hapCompanions'] = ['arbitrary-manifest.json']
        with self.assertRaisesRegex(ValueError, 'Unapproved HAP'): self.restore()
        self.assertEqual(self.calls, [])

    def test_final_hap_requires_native_companions(self):
        self.add_native_companions()
        self.restore(); self.write_lock()
        for corrupt in (False, True):
            hap = self.root / 'native.hap'
            with zipfile.ZipFile(hap, 'w') as archive:
                for path in self.raw.rglob('*'):
                    if path.is_file():
                        data = b'corrupt' if corrupt and path.name == 'libsteamohos_native_vulkan.so' else path.read_bytes()
                        archive.writestr('resources/rawfile/' + path.relative_to(self.raw).as_posix(), data)
            if corrupt:
                with self.assertRaisesRegex(ValueError, 'HAP companion'): components.verify_hap(hap, self.root)
            else:
                self.assertEqual(components.verify_hap(hap, self.root)['status'], 'PASS')

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
