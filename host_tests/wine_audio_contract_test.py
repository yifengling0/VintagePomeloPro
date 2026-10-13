"""Packaging must reject old audio libraries even when their ELF is valid."""
import ctypes
import io
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from zipfile import ZipFile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from runtime_library_guard import RuntimeLibraryError, validate_archive, validate_library
from wine_audio_contract import shared_audio_format


class WineAudioContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.folder = Path(cls.temp.name)
        # WAVEFORMATEXTENSIBLE is packed: 18-byte Format, 2-byte Samples,
        # 4-byte channel mask, 16-byte subtype GUID.
        cls.record = struct.pack('<HHIIHHHHIIHH8s', 0xfffe, 2, 48000, 384000,
            8, 32, 22, 32, 3, 3, 0, 0x10, bytes.fromhex('800000aa00389b71'))
        variants = {'float': cls.record,
                    's16': struct.pack('<HHIIHHHHIIHH8s', 0xfffe, 2, 48000, 192000,
                        4, 16, 22, 16, 3, 1, 0, 0x10, bytes.fromhex('800000aa00389b71')),
                    'pcm32': cls.record[:24] + b'\x01' + cls.record[25:],
                    'bad_align': cls.record[:12] + b'\x04' + cls.record[13:]}
        cls.binaries = {}
        for name, record in variants.items():
            source = ('#include <string.h>\n'
                '__attribute__((visibility("default"))) const unsigned char '
                'winehua_shared_audio_format_v1[] = {' + ','.join(map(str, record)) + '};\n'
                '__attribute__((visibility("default"))) void get_mix_format(void *out) '
                '{ memcpy(out, winehua_shared_audio_format_v1, sizeof(winehua_shared_audio_format_v1)); }\n')
            path = cls.folder / (name + '.so')
            subprocess.run(['cc', '-shared', '-fPIC', '-fvisibility=hidden', '-x', 'c',
                            '-', '-o', str(path)], input=source.encode(), check=True)
            cls.binaries[name] = path.read_bytes()
        subprocess.run(['strip', '--strip-all', str(cls.folder / 'float.so')], check=True)
        cls.stripped = (cls.folder / 'float.so').read_bytes()

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_compiled_and_stripped_format_matches_runtime_copy(self):
        lib = ctypes.CDLL(str(self.folder / 'float.so'))
        output = ctypes.create_string_buffer(40)
        lib.get_mix_format(output)
        self.assertEqual(output.raw, self.record)
        for data in (self.binaries['float'], self.stripped):
            self.assertEqual(shared_audio_format(data)['format'], 'IEEE_FLOAT')
            validate_library(data, 'libs/arm64-v8a/wineohos.so')

    def test_s16_and_inconsistent_formats_rejected(self):
        for name in ('s16', 'pcm32', 'bad_align'):
            with self.subTest(name=name), self.assertRaisesRegex(RuntimeLibraryError, 'Float32'):
                validate_library(self.binaries[name], 'wineohos.so')

    def test_old_library_without_contract_rejected(self):
        data = self.binaries['float'].replace(b'winehua_shared_audio_format_v1', b'legacyx_shared_audio_format_v1')
        with self.assertRaisesRegex(RuntimeLibraryError, 'missing shared Float32'):
            validate_library(data, 'wineohos.so')

    def test_malformed_audio_rejected(self):
        for data in (b'', self.stripped[:64], self.stripped[:-64]):
            with self.assertRaises(RuntimeLibraryError):
                validate_library(data, 'wineohos.so')

    def test_final_hap_and_nested_payload_checked(self):
        def archive(entries):
            buffer = io.BytesIO()
            with ZipFile(buffer, 'w') as output:
                for name, data in entries.items(): output.writestr(name, data)
            return buffer.getvalue()
        for name in ('float', 's16'):
            for entries in (
                {'libs/arm64-v8a/wineohos.so': self.binaries[name]},
                {'resources/rawfile/wine-data.zip': archive({'bin/x86_64-unix/wineohos.so': self.binaries[name]})}):
                if name == 'float':
                    validate_archive(archive(entries), 'candidate.hap')
                else:
                    with self.assertRaises(RuntimeLibraryError):
                        validate_archive(archive(entries), 'stale-baseline.hap')


if __name__ == '__main__':
    unittest.main()
