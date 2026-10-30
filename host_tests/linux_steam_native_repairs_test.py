#!/usr/bin/env python3
"""Exercise checksum and executable scope of the HAP-owned native repair pair."""
import hashlib, os, subprocess, tempfile, unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
class Repairs(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.temp=tempfile.TemporaryDirectory();cls.root=Path(cls.temp.name)
  source=cls.root/'probe.cpp'
  source.write_text(r'''
#include "native_fex_core.h"
#include <iostream>
int main(int n,char** v) {
 if(n!=3)return 64;
 if(!strcmp(v[1],"hash"))std::cout<<steamohos::fex::FileSha256(v[2]);
 else if(!strcmp(v[1],"helper"))std::cout<<steamohos::fex::Helper(v[2]);
 else if(!strcmp(v[1],"select")) {
  steamohos::fex::Plan p;p.tool=v[2];p.argv={"original","guest"};const char* reason=nullptr;
  bool selected=steamohos::fex::SelectCore(p,183,&reason);
  std::cout<<selected<<":"<<p.argv[0]<<":"<<reason;
 } else return 65;
}
''')
  cls.probe=cls.root/'probe'
  subprocess.run(['g++','-std=c++17','-O2','-Wall','-Wextra','-Werror',
   '-I'+str(ROOT/'entry/src/main/cpp/linux_steam/gnu_fixture'),str(source),'-ldl','-o',str(cls.probe)],check=True)
 @classmethod
 def tearDownClass(cls):cls.temp.cleanup()
 def run_probe(self,op,path,env=None):
  return subprocess.check_output([str(self.probe),op,str(path)],env={**os.environ,**(env or {})},text=True,timeout=10)
 def test_sha_matches_reference_and_rejects_links(self):
  for data in [b'abc',bytes(range(256))*513]:
   file=self.root/'bytes';file.write_bytes(data)
   self.assertEqual(self.run_probe('hash',file),hashlib.sha256(data).hexdigest())
  link=self.root/'link';link.symlink_to(file)
  self.assertEqual(self.run_probe('hash',link),'')
  self.assertEqual(self.run_probe('hash',self.root),'')
  self.assertEqual(self.run_probe('hash',self.root/'missing'),'')
 def test_registered_scope_and_unknown_depot(self):
  base=self.root/'scopes';code=base/'runtime';support=code/'native-game-support'
  support.mkdir(parents=True)
  for n in ['FEX','FEXServer']:(support/n).write_text('not executed')
  library=base/'library';tool=library/'steamapps/common/FEX-Emu';(tool/'usr/bin').mkdir(parents=True)
  (tool/'usr/bin/FEX').write_text('different official revision')
  env={'STEAMOHOS_CODE_ROOT':str(code),'STEAMOHOS_LIBRARY_ROOT':str(library),
   'STEAMOHOS_NATIVE_FEX_TOOL':str(tool),'STEAMOHOS_NATIVE_FEX':'1'}
  self.assertEqual(self.run_probe('helper',support/'FEX',env),'1')
  self.assertEqual(self.run_probe('helper',support/'FEXServer',env),'1')
  self.assertEqual(self.run_probe('helper',support/'FEX',{**env,'STEAMOHOS_NATIVE_FEX':'0'}),'0')
  outside=base/'outside';outside.write_text('untrusted')
  (support/'FEX').unlink();(support/'FEX').symlink_to(outside)
  self.assertEqual(self.run_probe('helper',support/'FEX',env),'0')
  self.assertEqual(self.run_probe('select',tool,env),'0:original:depot_identity')
  support.rename(code/'real-support');support.symlink_to(code/'real-support',target_is_directory=True)
  self.assertEqual(self.run_probe('helper',support/'FEXServer',env),'0')
  self.assertEqual(self.run_probe('select',tool,env),'0:original:support_scope')
if __name__=='__main__':unittest.main()
