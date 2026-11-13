# Exercise the actual Actions checkout shell with transient/permanent Git failures.
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import textwrap
import unittest

ROOT = Path(__file__).resolve().parents[1]
WORKFLOW = (ROOT / '.github/workflows/build.yml').read_text()
STEP = WORKFLOW.split('      - name: Fetch pinned submodules with bounded retries\n', 1)[1]
SCRIPT = textwrap.dedent(STEP.split('        run: |\n', 1)[1].split('\n      # Check exact gitlinks', 1)[0])
FAKE_GIT = '''
import json,os,sys
from pathlib import Path
with Path('commands.jsonl').open('a') as log:log.write(json.dumps(sys.argv[1:])+'\\n')
if 'sync' in sys.argv:
 if os.environ.get('FAKE_SYNC_ERROR'):print('fatal: bad submodule configuration');sys.exit(128)
 sys.exit(0)
assert 'update' in sys.argv and '--remote' not in sys.argv and '--force' not in sys.argv
p=Path('attempt');attempt=int(p.read_text())+1 if p.exists() else 1;p.write_text(str(attempt))
assert attempt==1 or Path('partial-object').exists()
Path('partial-object').write_text('reuse this object')
if attempt<=int(os.environ['FAKE_FAILURES']):
 print(os.environ['FAKE_ERROR'],file=sys.stderr);sys.exit(128)
print('checked out pinned recursive objects')
'''


class CheckoutRetries(unittest.TestCase):
    def run_case(self, failures, error='fatal: returned error: 503', sync_error=False):
        with tempfile.TemporaryDirectory(prefix='vpp-checkout-test-') as tmp:
            folder=Path(tmp);binary=folder/'bin';binary.mkdir()
            git=binary/'git';git.write_text('#!'+sys.executable+'\n'+FAKE_GIT);git.chmod(0o755)
            sleep=binary/'sleep';sleep.write_text('#!/bin/sh\nprintf "%s\\n" "$@" >> delays\n');sleep.chmod(0o755)
            env={**os.environ,'PATH':str(binary)+os.pathsep+os.environ['PATH'],
                 'RUNNER_TEMP':str(folder),'FAKE_FAILURES':str(failures),'FAKE_ERROR':error}
            if sync_error:env['FAKE_SYNC_ERROR']='1'
            result=subprocess.run(['bash','-c',SCRIPT],cwd=folder,env=env,text=True,capture_output=True,timeout=10)
            count=int((folder/'attempt').read_text()) if (folder/'attempt').exists() else 0
            commands=[json.loads(line) for line in (folder/'commands.jsonl').read_text().splitlines()]
            for args in commands[1:]:
                self.assertIn('--init',args);self.assertIn('--recursive',args)
                self.assertIn('--depth=1',args);self.assertNotIn('--remote',args);self.assertNotIn('--force',args)
            delays=(folder/'delays').read_text().splitlines() if (folder/'delays').exists() else []
            self.assertFalse((folder/'vpp-submodule-fetch-tail.log').exists(),'bounded diagnostic tail is cleaned')
            return result,count,delays

    def test_success_needs_one_attempt(self):
        result,count,delays=self.run_case(0)
        self.assertEqual((result.returncode,count,delays),(0,1,[]),result.stderr)

    def test_503_retries_and_reuses_partial_objects(self):
        result,count,delays=self.run_case(1)
        self.assertEqual((result.returncode,count,delays),(0,2,['15']),result.stderr)

    def test_transient_retries_are_bounded(self):
        result,count,delays=self.run_case(99)
        self.assertEqual((result.returncode,count,delays),(128,5,['15']*4),result.stderr)

    def test_tls_disconnect_is_transient(self):
        result,count,delays=self.run_case(1,'fatal: TLS connection was non-properly terminated')
        self.assertEqual((result.returncode,count,delays),(0,2,['15']),result.stderr)

    def test_permanent_failures_do_not_retry(self):
        for error in ('fatal: returned error: 404','fatal: Authentication failed',
                      'fatal: returned error: 403','fatal: not our ref deadbeef','fatal: unrecognized failure'):
            with self.subTest(error=error):
                result,count,delays=self.run_case(99,error)
                self.assertEqual((result.returncode,count,delays),(128,1,[]),result.stderr)

    def test_sync_failure_does_not_fetch(self):
        result,count,delays=self.run_case(0,sync_error=True)
        self.assertEqual((result.returncode,count,delays),(128,0,[]),result.stderr)


if __name__ == '__main__':
    unittest.main()
