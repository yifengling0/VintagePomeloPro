import base64
from pathlib import Path
import sys
import tempfile
import subprocess
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from local_source_receipt import capture, verify_effective
from verify_ci_release import verify_source
from ci_release_test import init_repo, git, write, commit

class LocalSourceTests(unittest.TestCase):
    def test_freezes_and_reconstructs_effective_vendor_edits(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d)/'root';init_repo(root)
            child=root/'thirdparty/runtime';init_repo(child)
            git(root,'submodule','add','./thirdparty/runtime','thirdparty/runtime');commit(root)
            write(child/'AppScope/app.json5','changed source')
            write(child/'helper.h','new managed header')
            value=capture(root)
            self.assertEqual(value['sourceMode'],'local-effective-source')
            delta=value['effectiveSources']['thirdparty/runtime']
            self.assertIn(b'changed source',base64.b64decode(delta['delta']))
            self.assertEqual(base64.b64decode(delta['files']['helper.h']['data']),b'new managed header')
            verify_effective(root,value)
            with self.assertRaises(subprocess.CalledProcessError): verify_source(root,value['commit'],'')
            write(child/'helper.h','changed again')
            with self.assertRaisesRegex(ValueError,'changed during build'): verify_effective(root,value)
            write(root/'AppScope/app.json5','dirty root')
            with self.assertRaises(subprocess.CalledProcessError): capture(root)

if __name__=='__main__': unittest.main()
