# SPDX-License-Identifier: LGPL-2.1-or-later
"""Changed patch, header or module inputs must invalidate NSI packaging."""
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'scripts'))
from proton_network_contract import validate_network_build


class NetworkContract(unittest.TestCase):
    def test_build_identity_and_abi_fail_closed(self):
        for changed in ('none', 'source', 'patch', 'dependency', 'binary', 'abi'):
            with self.subTest(changed=changed), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                def write(name, data):
                    path = root/name
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_bytes(data)
                    return path
                patch = write('patches/wine-ge/fix.patch', b'locked patch\n')
                lock = dict(wineRevision='locked', unixCallSlots=7,
                            patches=[dict(path='patches/wine-ge/fix.patch')])
                lock_path = write('docs/linux-steam/gnu-proton-network.lock.json', json.dumps(lock).encode())
                script = write('scripts/build_linux_proton_network.py', b'build script\n')
                contract = write('scripts/proton_network_contract.py', b'contract\n')
                dependency = write('include/header.h', b'ABI header\n')
                elf = bytearray(64)
                elf[:6] = b'\x7fELF\x02\x01'
                elf[18:20] = b'\xb7\x00'
                binary = write('build/nsiproxy.so', elf)
                sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
                receipt = dict(wineRevision='locked', unixCallTables={'__wine_unix_call_funcs': 7},
                               inputs={str(p.resolve()): sha(p) for p in (lock_path, script, contract, patch)},
                               dependencies={str(dependency.resolve()): sha(dependency)},
                               bytes=len(elf), sha256=sha(binary))
                target = {'source':script,'patch':patch,'dependency':dependency,'binary':binary}.get(changed)
                if target:
                    target.write_bytes(target.read_bytes()+b'changed')
                if changed == 'abi':
                    receipt['unixCallTables']['__wine_unix_call_funcs'] = 8
                receipt_path = write('build/build.json', json.dumps(receipt).encode())
                if changed == 'none':
                    self.assertEqual(validate_network_build(receipt_path, root)[0], binary)
                else:
                    with self.assertRaises(ValueError):
                        validate_network_build(receipt_path, root)


if __name__ == '__main__':
    unittest.main()
