"""Compile the actual first-match optional dispatch, including earlier FEX branches."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'scripts'))
from proton_optional_syscalls import proton_syscall_assembly

class ProtonOptionalSyscalls(unittest.TestCase):
    def test_shared_native_fex_remains_unchanged(self):
        original=(ROOT/'entry/src/main/cpp/linux_steam/gnu_fixture/syscall_compat.S').read_text()
        changed=proton_syscall_assembly(original)
        self.assertEqual(changed.split('#elif defined(__x86_64__)')[1],original.split('#elif defined(__x86_64__)')[1])
        self.assertEqual(original.count('    b.eq SteamOhosFexOptionalCall'),2)
        self.assertNotIn('    b.eq SteamOhosFexOptionalCall',changed.split('#elif defined(__x86_64__)')[0])
        with self.assertRaises(ValueError):proton_syscall_assembly(original.replace('    b.eq SteamOhosFexOptionalCall','    b.eq changed'))

    def test_compiled_arm64_branches_go_to_enosys(self):
        compiler=os.environ.get('ARM64_CC') or shutil.which('aarch64-linux-gnu-gcc')
        if not compiler:self.skipTest('ARM64 assembler unavailable')
        objdump=Path(compiler).with_name(Path(compiler).name.replace('gcc','objdump'))
        if not objdump.exists():self.skipTest('ARM64 objdump unavailable')
        with tempfile.TemporaryDirectory() as temporary:
            source=Path(temporary)/'syscall.S';binary=source.with_suffix('.o')
            source.write_text(proton_syscall_assembly((ROOT/'entry/src/main/cpp/linux_steam/gnu_fixture/syscall_compat.S').read_text()))
            subprocess.run([compiler,'-c',str(source),'-o',str(binary)],check=True)
            assembly=subprocess.check_output([str(objdump),'-dr',str(binary)],text=True)
            # The exact generated object has THREE direct error branches:
            # 374, 449 and 282. Earlier FEX-conditional forwarding is absent.
            self.assertEqual(assembly.count('R_AARCH64_CONDBR19\tSteamOhosSyscallUnavailable'),3)
            self.assertNotIn('R_AARCH64_CONDBR19\tSteamOhosFexOptionalCall',assembly)

if __name__=='__main__':unittest.main()
