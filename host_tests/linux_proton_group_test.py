#!/usr/bin/env python3
import json,subprocess,tempfile,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
class Group(unittest.TestCase):
 def test_complete_identity_interrupt_and_runtime_change(self):
  with tempfile.TemporaryDirectory(prefix='vpp-group-') as temp:
   root=Path(temp);probe=root/'probe.cpp'
   probe.write_text('#include "component_group.h"\n#include <iostream>\nint main(int n,char**v){if(n!=2)return 64;std::cout<<steamohos::CurrentComponentGroup(v[1]);return steamohos::ComponentGroupReady(v[1])?0:1;}')
   subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-I'+str(ROOT/'entry/src/main/cpp/linux_steam'),probe,'-o',root/'probe'],check=True)
   base=root/'base';active=base/'proton-probe';active.mkdir(parents=True)
   def write(path,data):path.parent.mkdir(parents=True,exist_ok=True);path.write_text(json.dumps(data,separators=(',',':')))
   for name in ['component-installed.json','media-installed.json','modules-installed.json','dxvk-support/manifest.json']:write(active/name,{'id':'a'*64})
   selection=base/'proton-runtimes/selection.json';write(selection,{'activeId':'b'*64})
   def run():return subprocess.run([root/'probe',active],capture_output=True,text=True,timeout=5)
   result=run();self.assertEqual(result.returncode,1);self.assertTrue(result.stdout)
   marker=active/'component-group-ready.json';marker.write_text(result.stdout);self.assertEqual(run().returncode,0)
   write(selection,{'activeId':'c'*64});self.assertEqual(run().returncode,1)
   marker.write_text(run().stdout);self.assertEqual(run().returncode,0)
   (active/'media-installed.json').unlink();self.assertEqual(run().returncode,1)
   write(active/'media-installed.json',{'id':'a'*64});marker.write_text(run().stdout)
   (active/'modules-installed.json').write_text('{"id":"'+('a'*64)+'","id":"'+('b'*64)+'"}')
   self.assertEqual(run().returncode,1)
if __name__=='__main__':unittest.main()
