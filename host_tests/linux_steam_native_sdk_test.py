"""Native Steam SDK delivery/identity tests, with tiny offline ELF fixtures."""
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('native_sdk', ROOT/'scripts/linux_steam_native_sdk.py')
sdk = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sdk)


class NativeSdkTests(unittest.TestCase):
    def fixture(self, root, wrong_elf=False):
        lock = json.loads(sdk.LOCK.read_text())
        archive = root/'official.zip'
        with zipfile.ZipFile(archive, 'w') as output:
            for i, row in enumerate(lock['files']):
                arch = row['machine'] if not wrong_elf else 183
                data = b'\x7fELF'+bytes([1 if arch == 3 else 2, 1])+bytes(12)+arch.to_bytes(2, 'little')+bytes([i])*20
                row.update(bytes=len(data),sha256=hashlib.sha256(data).hexdigest())
                output.writestr(row['source'], data)
        lock['archive'].update(bytes=archive.stat().st_size,sha256=sdk.digest(archive),
                               sha1=sdk.digest(archive,'sha1'),unpackedBytes=sum(f['bytes'] for f in lock['files']))
        path = root/'lock.json';path.write_text(json.dumps(lock))
        plan = dict(id='old-host', releaseManifest='{"files":[],"executionModel":"native_aarch64"}\n',
                    files=[dict(path='client/steamui/index.html',bytes=2,archive='gnu-runtime')],
                    archives=[dict(id='gnu-runtime',url='rawfile://gnu.zip',bytes=5,unpackedBytes=5)])
        return plan,archive,path

    def test_idempotence_and_sdk_identity_are_part_of_release(self):
        with tempfile.TemporaryDirectory() as tmp:
            args = self.fixture(Path(tmp))
            result = sdk.apply_sdk(*args)
            self.assertEqual(result,sdk.apply_sdk(result,*args[1:]))
            self.assertEqual(result['id'],hashlib.sha256(result['releaseManifest'].encode()).hexdigest())
            self.assertEqual(result['downloadBytes'],args[1].stat().st_size)
            self.assertEqual(len(result['files']),6)
            self.assertEqual(len(json.loads(result['releaseManifest'])['nativeSteamSdk']['files']),5)
            self.assertEqual(args[0]['id'],'old-host','input plan is not mutated before success')

    def test_corrupt_archive_and_arm_library_cannot_publish(self):
        with tempfile.TemporaryDirectory() as tmp:
            args = self.fixture(Path(tmp))
            with args[1].open('ab') as output:output.write(b'corrupt')
            with self.assertRaisesRegex(ValueError,'checksum'):sdk.apply_sdk(*args)
            args = self.fixture(Path(tmp),wrong_elf=True)
            with self.assertRaisesRegex(ValueError,'ELF/hash'):sdk.apply_sdk(*args)

    def test_production_plan_contains_both_architectures_in_home_sdk(self):
        plan=json.loads((ROOT/'entry/src/main/resources/rawfile/linux-steam/deployment.json').read_text())
        sdk.verify_plan(plan)
        lock=json.loads(sdk.LOCK.read_text())
        self.assertEqual(len([f for f in plan['files'] if f['path'].startswith('sdk/')]),5)
        for row in lock['files']:
            file=next(f for f in plan['files'] if f['path']==row['path'])
            self.assertEqual(file['sha256'],row['sha256'])
            self.assertEqual(file['bytes'],row['bytes'])
            self.assertEqual(file['archive'],'steam-native-sdk')
        self.assertEqual(next(a for a in plan['archives'] if a['id']=='steam-native-sdk')['sha256'],lock['archive']['sha256'])

    def test_incomplete_or_mixed_delivery_cannot_pass_build_gate(self):
        plan=json.loads((ROOT/'entry/src/main/resources/rawfile/linux-steam/deployment.json').read_text())
        for field in ('files', 'archives', 'releaseManifest'):
            broken=json.loads(json.dumps(plan))
            if field=='files':
                broken[field]=[f for f in broken[field] if f['path']!='sdk/sdk64/steamclient.so']
            elif field=='archives':
                next(a for a in broken[field] if a['id']=='steam-native-sdk')['sha256']='0'*64
            else:
                release=json.loads(broken[field]);release.pop('nativeSteamSdk');broken[field]=json.dumps(release)
            with self.assertRaisesRegex(ValueError,'Native Steam SDK'):sdk.verify_plan(broken)


if __name__=='__main__':unittest.main()
