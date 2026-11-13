"""Synthetic ELF SDK stub vs real implementation; safe generated-output reuse."""
import io
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from zipfile import ZipFile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from runtime_library_guard import RuntimeLibraryError, validate_archive, validate_library
from copy_runtime_library import copy_runtime


class RuntimeLibraryGuardTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(); self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.ext, self.sdk, self.dest = [self.root / name for name in ('ext', 'sdk', 'generated')]
        for path in (self.ext, self.sdk, self.dest): path.mkdir()
        names = ('inflate', 'inflateInit2_', 'inflateEnd', 'crc32', 'deflate')
        stub = 'int stub(void) { return 0; }\n' + '\n'.join(
            f'int {name}(void) __attribute__((alias("stub")));' for name in names)
        real = '\n'.join(f'int {name}(void) {{ return {index}; }}' for index, name in enumerate(names))
        for name, source in [('stub', stub), ('real', real)]:
            (self.root / (name + '.c')).write_text(source)
            subprocess.run(['cc', '-shared', '-fPIC', str(self.root / (name + '.c')),
                            '-o', str(self.root / (name + '.so'))], check=True)
        self.stub, self.real = [(self.root / (name + '.so')).read_bytes() for name in ('stub', 'real')]
        (self.sdk / 'libz.so').write_bytes(self.stub)

    def copy(self, system=True):
        copy_runtime(self.ext, self.sdk, self.dest, 'libz.so', 'libz.so', system=system)

    def test_real_ext_runtime_preserved(self):
        (self.ext / 'libz.so').write_bytes(self.real)
        self.copy(); self.assertEqual((self.dest / 'libz.so').read_bytes(), self.real)

    def test_ext_sdk_copy_rejected(self):
        (self.ext / 'libz.so').write_bytes(self.stub)
        with self.assertRaises(RuntimeLibraryError): self.copy()
        self.assertTrue((self.ext / 'libz.so').exists())

    def test_ext_sdk_symlink_rejected(self):
        (self.ext / 'libz.so').symlink_to(self.sdk / 'libz.so')
        with self.assertRaises(RuntimeLibraryError): self.copy()

    def test_native_stale_sdk_copy_removed_only_from_generated(self):
        (self.dest / 'libz.so').write_bytes(self.stub)
        self.copy(); self.assertFalse((self.dest / 'libz.so').exists())
        self.assertEqual((self.sdk / 'libz.so').read_bytes(), self.stub)

    def test_unknown_stale_file_is_preserved_and_blocks(self):
        (self.dest / 'libz.so').write_bytes(b'user modified')
        with self.assertRaises(RuntimeLibraryError): self.copy()
        self.assertEqual((self.dest / 'libz.so').read_bytes(), b'user modified')

    def test_guest_requires_real_runtime(self):
        with self.assertRaises(RuntimeLibraryError): self.copy(system=False)

    def test_direct_and_nested_hap_stub_rejected(self):
        def archive(entries):
            data = io.BytesIO()
            with ZipFile(data, 'w') as out:
                for name, contents in entries.items(): out.writestr(name, contents)
            return data.getvalue()
        for entries in ({'libs/arm64-v8a/libz.so': self.stub},
                        {'resources/rawfile/wine-data.zip': archive({'guest_gfx/x86_64/lib/libz.so': self.stub})}):
            with self.assertRaises(RuntimeLibraryError): validate_archive(archive(entries), 'test.hap')
        validate_archive(archive({'libs/arm64-v8a/libz.so': self.real}), 'real.hap')

    def test_gates_cover_assemble_and_unsigned_hap(self):
        assemble = (ROOT / 'scripts/assemble.sh').read_text()
        package = (ROOT / 'scripts/package.sh').read_text()
        self.assertEqual(assemble.count('copy_runtime_library.py'), 3)
        routing = (ROOT / 'entry/src/main/cpp/wine/wine_env.cpp').read_text()
        self.assertEqual(routing.count('libxml2.so:libxml2.so.2:libz.so:libz.so.1'), 2)
        self.assertIn('runtime_library_guard.py" "$NATIVE_LIBS" "$wine_data"', assemble)
        self.assertLess(package.index('runtime_library_guard.py" "$unsigned_hap"'),
                        package.index('if [ "$mode" = unsigned ]; then', package.index('hvigorw assembleHap')))


if __name__ == '__main__': unittest.main()
