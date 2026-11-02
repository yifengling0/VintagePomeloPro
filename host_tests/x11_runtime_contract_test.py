"""Exercise the production X11 closure gate with real ELF dependency fixtures."""
import io
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from zipfile import ZipFile
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from check_runtime_components import ValidationError
from runtime_library_guard import RuntimeLibraryError
import verify_x11_runtime as gate
import verify_x11_wine_config as config


def pe(machine):
    data = bytearray(512); data[:2] = b'MZ'
    struct.pack_into('<I', data, 60, 128); data[128:132] = b'PE\0\0'
    struct.pack_into('<H', data, 132, machine)
    return bytes(data)


def payload(overrides=None):
    files = dict.fromkeys(gate.DATA, b'data')
    files['xkm/server-0.xkm'] = b'\x0fmkx' + bytes(16)
    files['bin/aarch64-windows/winex11.drv'] = pe(0xaa64)
    files['bin/i386-windows/winex11.drv'] = pe(0x14c)
    files.update(overrides or {})
    stream = io.BytesIO()
    with ZipFile(stream, 'w') as archive:
        for name, data in files.items():
            if data is not None: archive.writestr(name, data)
    stream.seek(0)
    return stream


class RuntimeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='x11-contract-')
        root = Path(cls.temp.name)
        source = root / 'fixture.c'; source.write_text('int value(void) { return 42; }\n')
        subprocess.run(['cc','-shared','-nostdlib','-fPIC', str(source),
            '-Wl,-soname,libfixture.so', '-o', str(root/'libfixture.so')], check=True)
        cls.native = (root/'libfixture.so').read_bytes()
        def arm(data):
            result = bytearray(data); struct.pack_into('<H', result, 18, 183); return bytes(result)
        cls.arm = arm(cls.native)
        def dependent(soname):
            subprocess.run(['cc','-shared','-nostdlib','-fPIC', str(source),
                '-Wl,-soname,'+soname, '-o', str(root/'dep.so')], check=True)
            subprocess.run(['cc','-shared','-nostdlib','-fPIC', str(source),
                '-Wl,--no-as-needed', str(root/'dep.so'), '-o', str(root/'main.so')], check=True)
            return arm((root/'main.so').read_bytes())
        cls.dependent = dependent('libfixture.so')
        cls.gnu = dependent('libc.so.6')

    @classmethod
    def tearDownClass(cls): cls.temp.cleanup()

    def verify(self, files):
        return gate.verify_libraries(files.__getitem__, files.keys(), 'aarch64', ('main.so',))

    def test_transitive_dependency_and_missing_library(self):
        self.assertEqual(self.verify({'main.so':self.dependent,'libfixture.so':self.arm}),
                         {'main.so','libfixture.so'})
        with self.assertRaisesRegex(ValidationError, 'unresolved'):
            self.verify({'main.so':self.dependent})

    def test_gnu_abi_and_wrong_architecture(self):
        with self.assertRaisesRegex(ValidationError, 'GNU ABI'):
            self.verify({'main.so':self.gnu})
        with self.assertRaisesRegex(ValueError, 'expected aarch64'):
            self.verify({'main.so':self.native})

    def test_payload_requires_both_pe_architectures(self):
        gate.verify_payload(payload(), 'aarch64')
        for name, value in [('bin/i386-windows/winex11.drv',pe(0x8664)),
                            ('bin/aarch64-windows/winex11.drv',pe(0x8664))]:
            with self.assertRaisesRegex(ValidationError, 'architecture'):
                gate.verify_payload(payload({name:value}), 'aarch64')

    def test_xkb_locale_and_xkm_are_mandatory(self):
        for name in gate.DATA:
            with self.assertRaises(KeyError): gate.verify_payload(payload({name:None}), 'aarch64')
        with self.assertRaisesRegex(ValidationError, 'invalid XKM'):
            gate.verify_payload(payload({'xkm/server-0.xkm':b'bad'}), 'aarch64')

    def test_config_rejects_silent_feature_disable_or_wrong_soname(self):
        header = '\n'.join('#define '+name+' '+value for name,value in config.REQUIRED.items())
        config.verify(header)
        for name,value in config.REQUIRED.items():
            with self.assertRaisesRegex(ValueError, name):
                config.verify(header.replace('#define '+name+' '+value,'/* #undef '+name+' */'))
        with self.assertRaisesRegex(ValueError,'SONAME_LIBXI'):
            config.verify(header.replace('libXi.so.6','libXi.so.99'))

if __name__ == '__main__': unittest.main()
