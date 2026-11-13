"""Compile real winecrt0 with implicit SEH and inspect the linked PE fallback.

Requires the project's llvm-mingw toolchain; no Windows execution is claimed.
"""
import argparse
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--wine-src', type=Path, required=True)
parser.add_argument('--toolchain', type=Path, required=True)
options, remaining = parser.parse_known_args()
TOOLS = options.toolchain
WINE = options.wine_src


def run(args):
    return subprocess.run(list(map(str, args)), check=True, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE).stdout


class PE:
    def __init__(self, path):
        self.data = path.read_bytes()
        coff = struct.unpack_from('<I', self.data, 0x3c)[0] + 4
        count, size = struct.unpack_from('<H12xH', self.data, coff + 2)
        opt = coff + 20
        self.base = struct.unpack_from('<Q', self.data, opt + 24)[0]
        self.import_rva = struct.unpack_from('<I', self.data, opt + 120)[0]
        self.sections = []
        for i in range(count):
            pos = opt + size + i * 40
            virtual_size, rva, raw_size, raw, flags = struct.unpack_from(
                '<IIII12xI', self.data, pos + 8)
            self.sections.append((rva, max(virtual_size, raw_size), raw, flags))

    def offset(self, rva):
        for start, size, raw, _ in self.sections:
            if start <= rva < start + size:
                return raw + rva - start
        raise AssertionError(f'RVA {rva:#x} is outside PE sections')

    def string(self, rva):
        pos = self.offset(rva)
        return self.data[pos:self.data.index(b'\0', pos)].decode()

    def imports(self):
        result = set()
        if not self.import_rva:
            return result
        pos = self.offset(self.import_rva)
        while any(self.data[pos:pos + 20]):
            lookup, _, _, name, iat = struct.unpack_from('<IIIII', self.data, pos)
            module = self.string(name)
            table = self.offset(lookup or iat)
            while (entry := struct.unpack_from('<Q', self.data, table)[0]):
                result.add((module, entry if entry >> 63 else self.string(entry + 2)))
                table += 8
            pos += 20
        return result

    def executable(self, rva):
        return any(start <= rva < start + size and flags & 0x20000000
                   for start, size, _, flags in self.sections)

    def fallback(self, thunk_rva):
        pos = self.offset(thunk_rva)
        adrp, add = struct.unpack_from('<II', self.data, pos + 8)
        # LLD ImportThunkChunkARM64EC sets x10 to the exit thunk in this pair.
        assert adrp & 0x9f00001f == 0x9000000a
        assert add & 0xffc003ff == 0x9100014a
        immediate = ((adrp >> 5) & 0x7ffff) << 2 | ((adrp >> 29) & 3)
        if immediate & (1 << 20):
            immediate -= 1 << 21
        return ((thunk_rva + 8) & ~0xfff) + immediate * 4096 + ((add >> 10) & 0xfff)


PROBE = '''
#if LOCAL_HANDLER
int __C_specific_handler(void *a, void *b, void *c, void *d) { return 0; }
#else
__declspec(dllexport) int probe(volatile int *p) {
#if USE_SEH
    __try { return *p; } __except(1) { return 17; }
#else
    return *p;
#endif
}
#endif
'''


class SehExitThunkTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='seh-exit-thunk-')
        cls.addClassCleanup(cls.temp.cleanup)
        cls.work = Path(cls.temp.name)
        source = (WINE / 'dlls/winecrt0/arm64ec.c').read_text()
        begin = source.index('/* Compiler-generated SEH references')
        end = source.index('#endif /* __arm64ec__ */', begin)
        for label, text in [('before', source[:begin] + source[end:]), ('after', source)]:
            path = cls.work / (label + '.c')
            path.write_text(text)
            cls.compile(path, cls.work / (label + '.o'), 'arm64ec', support=True)
        (cls.work / 'probe.c').write_text(PROBE)
        (cls.work / 'handler.def').write_text(
            'LIBRARY vcruntime140.dll\nEXPORTS\n __C_specific_handler\n memcpy\n memmove\n memset\n')
        for arch in ['arm64ec', 'arm64']:
            run([TOOLS / 'llvm-dlltool', '-m', arch, '-d', cls.work / 'handler.def',
                 '-l', cls.work / (arch + '.lib')])

    @classmethod
    def compile(cls, source, output, arch, support=False, defines=()):
        run([TOOLS / 'clang', '-target', arch + '-windows', '--no-default-config',
             '-O2', *([] if support or 'LOCAL_HANDLER=1' in defines else ['-fasync-exceptions']),
             '-D__WINE_PE_BUILD', '-D__WINESRC__', '-I', WINE / 'include',
             '-I', WINE / 'include/msvcrt', *['-D' + d for d in defines],
             '-c', source, '-o', output])

    def link(self, label, seh=True, local=False, dual=False):
        tag = f'{label}-{seh}-{local}-{dual}'
        ec = self.work / (tag + '-ec.o')
        self.compile(self.work / 'probe.c', ec, 'arm64ec', defines=[f'USE_SEH={int(seh)}'])
        inputs = [ec, self.work / (label + '.o')]
        if local:
            obj = self.work / (tag + '-local.o')
            self.compile(self.work / 'probe.c', obj, 'arm64ec', defines=['LOCAL_HANDLER=1'])
            inputs.append(obj)
        if dual:
            obj = self.work / (tag + '-native.o')
            self.compile(self.work / 'probe.c', obj, 'aarch64', defines=[f'USE_SEH={int(seh)}'])
            inputs.append(obj)
            if local:
                obj = self.work / (tag + '-local-native.o')
                self.compile(self.work / 'probe.c', obj, 'aarch64', defines=['LOCAL_HANDLER=1'])
                inputs.append(obj)
        inputs.append(self.work / 'arm64ec.lib')
        if dual:
            inputs.append(self.work / 'arm64.lib')
        output, mapfile = self.work / (tag + '.dll'), self.work / (tag + '.map')
        run([TOOLS / 'lld-link', '/machine:' + ('arm64x' if dual else 'arm64ec'),
             '/dll', '/noentry', '/out:' + str(output), '/map:' + str(mapfile), *inputs])
        pe = PE(output)
        match = re.search(r'__impchk___C_specific_handler\s+([0-9a-fA-F]+)', mapfile.read_text())
        return pe, None if match is None else int(match[1], 16) - pe.base

    def assert_valid_fallback(self, pe, thunk):
        self.assertIsNotNone(thunk)
        target = pe.fallback(thunk)
        self.assertNotEqual(target, 0)
        self.assertTrue(pe.executable(target))
        # The four integer/pointer arguments stay in x0-x3; x8 holds x64 RAX.
        code = struct.unpack_from('<10I', pe.data, pe.offset(target))
        self.assertEqual(code[:3], (0xd100c3ff, 0xa9027bfd, 0x910083fd))
        self.assertEqual(code[3] & 0x9f00001f, 0x90000008)  # adrp x8
        self.assertEqual(code[4] & 0xffc003ff, 0xf9400110)  # ldr x16, [x8, ...]
        self.assertEqual(code[5:], (0xd63f0200, 0xaa0803e0, 0xa9427bfd, 0x9100c3ff,
                                  0xd65f03c0))

    def test_implicit_personality_regression(self):
        pe, thunk = self.link('before')
        self.assertEqual(pe.fallback(thunk), 0)
        self.assert_valid_fallback(*self.link('after'))

    def test_arm64x_imported_personality(self):
        self.assert_valid_fallback(*self.link('after', dual=True))

    def test_local_personality_does_not_pull_duplicate_import(self):
        for dual in [False, True]:
            pe, thunk = self.link('after', local=True, dual=dual)
            self.assertIsNone(thunk)
            self.assertFalse(any(name == '__C_specific_handler' for _, name in pe.imports()))

    def test_no_seh_does_not_add_handler_dependency(self):
        before, _ = self.link('before', seh=False)
        after, thunk = self.link('after', seh=False)
        self.assertIsNone(thunk)
        self.assertEqual(before.imports(), after.imports())


if __name__ == '__main__':
    unittest.main(argv=[__file__, *remaining])
