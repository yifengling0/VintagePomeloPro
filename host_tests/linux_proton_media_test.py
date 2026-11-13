"""Media dependencies must fail before packaging, including dynamically loaded DLLs."""
from pathlib import Path
import sys
import unittest
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'scripts'))
from prepare_linux_proton_media import verify,RAW,MEDIA_LIBS
class MediaTests(unittest.TestCase):
    def test_real_pinned_media_closure(self):
        m=verify();self.assertGreater(len(m['files']),20)
        self.assertLess(m['installedBytes'],64*1024*1024)
    def test_each_missing_codec_rejected(self):
        read=Path.read_bytes
        for name in MEDIA_LIBS:
            target=ROOT/RAW/'files/lib/aarch64-linux-gnu'/name
            def missing(path):
                if path==target:raise FileNotFoundError(str(path))
                return read(path)
            with self.subTest(name=name),patch.object(Path,'read_bytes',missing):
                with self.assertRaises(FileNotFoundError):verify()
    def test_each_corrupt_codec_rejected(self):
        read=Path.read_bytes
        for name in MEDIA_LIBS:
            target=ROOT/RAW/'files/lib/aarch64-linux-gnu'/name
            def corrupt(path):return b'wrong architecture/content' if path==target else read(path)
            with self.subTest(name=name),patch.object(Path,'read_bytes',corrupt):
                with self.assertRaisesRegex(ValueError,'checksum'):verify()
    def test_missing_64bit_mf_and_directshow_rejected(self):
        read=Path.read_bytes
        for name in ('mfplat.dll','quartz.dll','mfplay.dll'):
            target=ROOT/RAW/'files/lib/wine/aarch64-windows'/name
            def missing(path):
                if path==target:raise FileNotFoundError(str(path))
                return read(path)
            with self.subTest(name=name),patch.object(Path,'read_bytes',missing):
                with self.assertRaises(FileNotFoundError):verify()
if __name__=='__main__':unittest.main()
