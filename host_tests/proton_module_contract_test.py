# SPDX-License-Identifier: LGPL-2.1-or-later
"""Missing/dynamic Wine modules, stale locks and hostile archive members."""
import copy
import hashlib
import io
import json
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'scripts'))
from proton_module_contract import DIRECTORIES, complete_module_lock, validate_module_lock


class ModuleContract(unittest.TestCase):
    def archive(self, base, names):
        archive = base/'ge.tar.gz'
        with tarfile.open(archive, 'w:gz') as output:
            for name in names:
                content = name.encode()
                member = tarfile.TarInfo('ge/'+name)
                member.size = len(content)
                output.addfile(member, io.BytesIO(content))
        lock = dict(distribution=dict(file=archive.name, bytes=archive.stat().st_size,
            sha512=hashlib.sha512(archive.read_bytes()).hexdigest()), selected=[])
        return archive, lock

    def test_complete_both_abis_including_unreferenced_dynamic_modules(self):
        with tempfile.TemporaryDirectory() as temporary:
            names = [directory+'/'+name for directory in DIRECTORIES
                     for name in ('core.dll', 'dynamically-loaded.dll')]
            archive, lock = self.archive(Path(temporary), names)
            completed = complete_module_lock(lock, archive)
            validate_module_lock(completed)
            self.assertEqual({row['path'] for row in completed['selected']}, set(names))
            self.assertEqual(complete_module_lock(completed, archive), completed)
            broken = copy.deepcopy(completed)
            broken['selected'].pop()
            with self.assertRaises(ValueError):
                validate_module_lock(broken)
            broken = copy.deepcopy(completed)
            broken['selected'][0]['sha256'] = '0'*64
            with self.assertRaises(ValueError):
                complete_module_lock(broken, archive)
            lock['distribution']['sha512'] = '0'*128
            with self.assertRaises(ValueError):
                complete_module_lock(lock, archive)

    def test_duplicate_and_parent_traversal_are_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            for bad in ([DIRECTORIES[0]+'/x.dll']*2,
                        [DIRECTORIES[0]+'/../outside.dll']):
                archive, lock = self.archive(Path(temporary), bad)
                with self.assertRaises(ValueError):
                    complete_module_lock(lock, archive)

    def test_shipped_lock_covers_whole_directories(self):
        lock = json.loads((ROOT/'docs/linux-steam/gnu-proton-modules.lock.json').read_text())
        validate_module_lock(lock)


if __name__ == '__main__':
    unittest.main()
