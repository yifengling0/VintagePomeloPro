import copy, hashlib, importlib.util, json, tempfile, unittest, zipfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('compat',ROOT/'scripts/import_linux_steam_client_compat.py');module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
class CompatibilityModules(unittest.TestCase):
 def test_only_pinned_linux_modules_are_imported_and_reproducible(self):
  with tempfile.TemporaryDirectory() as tmp:
   root=Path(tmp);package=root/'official.zip';records={};contents={}
   for name,source,arch in [('steamclient.dll','steamclient.dll','i386'),('steamclient64.dll','steamclient64.dll','x86_64'),('Steam.dll','legacycompat/Steam.dll','i386')]:
    data=bytearray(128);data[:2]=b'MZ';data[60:64]=(64).to_bytes(4,'little');data[64:70]=b'PE\0\0'+(b'\x4c\x01' if arch=='i386' else b'\x64\x86')
    contents[source]=data;records[name]={'source':source,'architecture':arch,'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest()}
   with zipfile.ZipFile(package,'w') as z:
    for n,b in contents.items():z.writestr(n,b)
    z.writestr('steam.exe',b'not imported');z.writestr('userdata/account',b'not imported')
   lock={'packageSha1':hashlib.sha1(package.read_bytes()).hexdigest(),'files':records}
   (root/'manifest.json').write_text(json.dumps({'steamClientProbe':{}}))
   first=module.import_compat(package,root,lock);self.assertEqual(first,module.import_compat(package,root,lock))
   with zipfile.ZipFile(root/'steam-client-compat.zip') as z:self.assertEqual(set(z.namelist()),{'steamohos/compat/'+n for n in records})
   for kind in ['package','hash','source','arch','allowlist']:
    bad=copy.deepcopy(lock)
    if kind=='package':bad['packageSha1']='0'*40
    elif kind=='hash':bad['files']['steamclient.dll']['sha256']='0'*64
    elif kind=='source':bad['files']['steamclient.dll']['source']='../steamclient.dll'
    elif kind=='arch':bad['files']['steamclient.dll']['architecture']='x86_64'
    else:bad['files']['steam.exe']=bad['files'].pop('Steam.dll')
    with self.assertRaises(ValueError):module.import_compat(package,root,bad)
if __name__=='__main__':unittest.main()
