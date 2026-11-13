#!/usr/bin/env python3
"""A fresh receipt cannot bless stale original inputs after source transforms."""
import hashlib,json,sys,tempfile,unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from prepare_linux_proton_dxvk import validate_probe_source_receipt

def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
class ReceiptTest(unittest.TestCase):
 def test_transformed_originals_and_headers_are_required(self):
  with tempfile.TemporaryDirectory() as tmp:
   root=Path(tmp);fixture=root/'entry/src/main/cpp/linux_steam/gnu_fixture';fixture.mkdir(parents=True)
   originals=['exec_compat.cpp','filesystem_compat.c','posix_compat.c','syscall_compat.S']
   plain=['sysvipc_compat.c','popen_compat.c','socket_metadata.c','launch_wire.h']
   paths=[fixture/n for n in originals+plain]+[fixture.parent/n for n in ['native_loader.cpp','native_loader_entry.S','runtime_lease.h']]
   for path in paths:path.write_text('// source '+path.name)
   shim=root/'out';shim.mkdir();(shim/'exec_probe.cpp').write_text('// generated')
   build={'buildInputs':{'originalSources':{str((fixture/n).resolve()):sha(fixture/n) for n in originals},
      'sources':{str(p.resolve()):sha(p) for p in paths if p.name not in originals}},
      'generatedSources':{'exec_probe.cpp':sha(shim/'exec_probe.cpp')}}
   self.assertIn(str((fixture/'exec_compat.cpp').relative_to(root)),validate_probe_source_receipt(shim,build,root))
   for p in paths+[shim/'exec_probe.cpp']:
    before=p.read_bytes();p.write_bytes(before+b' changed')
    with self.assertRaisesRegex(ValueError,'Stale|changed'):validate_probe_source_receipt(shim,build,root)
    p.write_bytes(before)
   build['buildInputs'].pop('originalSources')
   with self.assertRaisesRegex(ValueError,'original'):validate_probe_source_receipt(shim,build,root)
if __name__=='__main__':unittest.main()
