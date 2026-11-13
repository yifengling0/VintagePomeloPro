"""Actual font extractor + private Z_SOLO inflater, without OHOS/NAPI bindings."""
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
CPP = ROOT / 'entry/src/main/cpp'
VENDOR = CPP / 'steam_decompress/zlib_vendored'


class FontZipTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.work = Path(cls.temp.name)
        source = (CPP / 'common/font_zip.cpp').read_text().split('std::string ReadNapiString')[0]
        source = source.replace('#include "common/font_zip.h"', '').replace('#include <hilog/log.h>', '#define OH_LOG_INFO(...) ((void)0)')
        source += r'''
} // anonymous namespace
#include <iostream>
extern "C" void* __real_calloc(size_t, size_t);
extern "C" void* __wrap_calloc(size_t n, size_t size) {
    if (getenv("FONT_TEST_ALLOC_FAIL")) return nullptr;
    return __real_calloc(n, size);
}
extern "C" ssize_t __real_write(int, const void*, size_t);
extern "C" ssize_t __wrap_write(int fd, const void* p, size_t size) {
    if (getenv("FONT_TEST_WRITE_FAIL")) { errno=ENOSPC; return -1; }
    return __real_write(fd, p, size);
}
int main(int argc, char** argv) {
    if (argc!=3) return 2;
    auto r=ExtractFontZipImpl(argv[1], argv[2]);
    std::cout << r.ok << "|" << r.fonts << "|" << r.bad << "|" << ErrorCode(r.errorCode)
              << "|" << r.zlibCode << "|" << r.firstBadExt << "\n";
}
'''
        (cls.work / 'runner.cpp').write_text(source)
        objects = []
        for name in ('adler32', 'crc32', 'inflate', 'inftrees', 'inffast', 'zutil'):
            obj = cls.work / (name + '.o')
            subprocess.run(['cc', '-fPIC', '-O2', '-fvisibility=hidden', '-DZ_SOLO', '-c',
                            str(VENDOR / (name + '.c')), '-o', str(obj)], check=True)
            objects.append(str(obj))
        cls.runner = cls.work / 'font_zip'
        subprocess.run(['c++', '-std=c++17', '-DZ_SOLO', '-I' + str(CPP),
                        str(cls.work / 'runner.cpp'), *objects, '-Wl,--wrap=calloc',
                        '-Wl,--wrap=write', '-o', str(cls.runner)], check=True)
        cls.shared = cls.work / 'private.so'
        subprocess.run(['cc', '-shared', *objects, '-o', str(cls.shared)], check=True)
        cls.font = (ROOT / 'thirdparty/wine-valve/fonts/system.ttf').read_bytes()

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def setUp(self):
        self.path = self.work / (self._testMethodName + '.zip')
        self.out = self.work / (self._testMethodName + '-out')

    def archive(self, method=zipfile.ZIP_DEFLATED, count=1, payload=None, name='测试.ttf'):
        with zipfile.ZipFile(self.path, 'w', compression=method) as archive:
            for index in range(count):
                archive.writestr(f'dir{index}/{name}', self.font if payload is None else payload)

    def result(self, **env):
        result = subprocess.check_output([str(self.runner), str(self.path), str(self.out)],
                                         env={**os.environ, **env}, text=True).strip().split('|')
        return tuple(map(int, result[:3])) + tuple(result[3:])

    def test_deflate_344_real_fonts(self):
        self.archive(count=344)
        self.assertEqual(self.result()[:4], (1, 344, 0, ''))
        self.assertEqual(len(list(self.out.iterdir())), 344)
        self.assertTrue(all(p.read_bytes() == self.font for p in self.out.iterdir()))

    def test_store_real_font(self):
        self.archive(zipfile.ZIP_STORED)
        self.assertEqual(self.result()[:4], (1, 1, 0, ''))

    def test_nonfont_extension_only_is_bad(self):
        self.archive(name='readme.txt')
        self.assertEqual(self.result(), (1, 0, 1, '', '0', 'txt'))

    def test_bad_font_content_is_typed(self):
        self.archive(payload=b'not a font')
        self.assertEqual(self.result()[:4], (0, 0, 0, 'invalid_font'))

    def test_unsupported_compression(self):
        self.archive(zipfile.ZIP_BZIP2)
        self.assertEqual(self.result()[:4], (0, 0, 0, 'unsupported'))

    def test_inflate_initialization_failure(self):
        self.archive()
        self.assertEqual(self.result(FONT_TEST_ALLOC_FAIL='1')[:4], (0, 0, 0, 'inflate_init'))

    def test_corrupt_deflate(self):
        self.archive()
        data = bytearray(self.path.read_bytes())
        start = 30 + struct.unpack_from('<H', data, 26)[0]
        data[start:start+4] = b'\xff' * 4
        self.path.write_bytes(data)
        self.assertEqual(self.result()[:4], (0, 0, 0, 'decompress'))

    def test_crc_failure(self):
        self.archive(zipfile.ZIP_STORED)
        data = bytearray(self.path.read_bytes()); central = data.index(b'PK\x01\x02')
        struct.pack_into('<I', data, central + 16, 1)
        self.path.write_bytes(data)
        self.assertEqual(self.result()[:4], (0, 0, 0, 'decompress'))

    def test_read_failure(self):
        self.archive()
        data = bytearray(self.path.read_bytes()); central = data.index(b'PK\x01\x02')
        struct.pack_into('<I', data, central + 42, len(data) + 99)
        self.path.write_bytes(data)
        self.assertEqual(self.result()[:4], (0, 0, 0, 'read'))

    def test_size_limit(self):
        self.archive()
        data = bytearray(self.path.read_bytes()); central = data.index(b'PK\x01\x02')
        struct.pack_into('<I', data, central + 24, 64 * 1024 * 1024 + 1)
        self.path.write_bytes(data)
        self.assertEqual(self.result()[:4], (0, 0, 0, 'limit'))

    def test_write_failure_unlinks_partial(self):
        self.archive()
        self.assertEqual(self.result(FONT_TEST_WRITE_FAIL='1')[:4], (0, 0, 0, 'write'))
        self.assertEqual(list(self.out.iterdir()), [])

    def test_existing_file_not_overwritten(self):
        self.archive(name='old.ttf'); self.out.mkdir(); (self.out / 'old.ttf').write_bytes(b'original')
        self.assertEqual(self.result()[:4], (0, 0, 0, 'write'))
        self.assertEqual((self.out / 'old.ttf').read_bytes(), b'original')

    def test_directory_corruption_is_not_success(self):
        self.archive()
        data = bytearray(self.path.read_bytes()); central = data.index(b'PK\x01\x02')
        data[central] = 0; self.path.write_bytes(data)
        self.assertEqual(self.result()[:4], (0, 0, 0, 'archive'))

    def test_hidden_zlib_no_dynamic_dependency(self):
        symbols = subprocess.check_output(['readelf', '--dyn-syms', '--wide', str(self.shared)], text=True)
        dynamic = subprocess.check_output(['readelf', '-d', str(self.runner)], text=True)
        self.assertNotIn(' inflate', symbols)
        self.assertNotIn('libz.so', dynamic)
        cmake = (CPP / 'CMakeLists.txt').read_text()
        entry_links = cmake.split('target_link_libraries(entry PUBLIC', 1)[1].split(')', 1)[0]
        self.assertNotIn('libz.so', entry_links)
        self.assertIn('target_link_libraries(entry PRIVATE fontzip_zlib)', cmake)


if __name__ == '__main__':
    unittest.main()
