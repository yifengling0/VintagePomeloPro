"""Check the capability probe against real loopback sockets and the host lsof."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ConnectionIdentity(unittest.TestCase):
    @unittest.skipUnless(Path('/bin/lsof').is_file(), 'native lsof is required')
    def test_real_parent_and_child_identities(self):
        with tempfile.TemporaryDirectory(prefix='steamohos-connection-') as directory:
            binary = Path(directory) / 'probe'
            source = ROOT / 'entry/src/main/cpp/linux_steam/gnu_fixture/connection_identity.c'
            subprocess.run(['cc', '-O2', '-Wall', '-Wextra', '-Werror', str(source),
                            '-ldl', '-o', str(binary)], check=True)
            environment = {k: v for k, v in os.environ.items()
                           if not k.startswith('STEAMOHOS_') and k != 'LD_PRELOAD'}
            result = subprocess.run([str(binary)], capture_output=True, text=True,
                                    timeout=10, env=environment)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('parent_found=1 child_found=1 child_status=0', result.stderr)
            self.assertIn('role=child-client', result.stderr)
            self.assertIn('table path=/proc/net/tcp open_errno=0', result.stderr)
            self.assertIn('stat=0/0 mode=140', result.stderr)
            self.assertIn('lsof_status=0', result.stderr)


if __name__ == '__main__':
    unittest.main()
