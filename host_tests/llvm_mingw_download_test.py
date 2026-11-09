"""Exercise pinned toolchain caching and checksum enforcement without a large download."""
import hashlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
import urllib.error

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from download_llvm_mingw import download


class ToolchainDownloadTest(unittest.TestCase):
    def test_gateway_fallback_cache_and_corruption(self):
        payload = b'toolchain fixture'
        url = 'https://github.com/mstorsjo/llvm-mingw/releases/download/test/tool.tar.xz'
        dist = dict(source=url, file='tool.tar.xz', bytes=len(payload))
        digest = hashlib.sha256(payload).hexdigest()
        api = 'https://api.github.com/repos/mstorsjo/llvm-mingw/releases/'
        asset = api + 'assets/42'
        metadata = dict(tag_name='test', assets=[dict(name='tool.tar.xz', size=len(payload),
                                                     browser_download_url=url, url=asset)])
        for corrupt in (False, True):
            with self.subTest(corrupt=corrupt), tempfile.TemporaryDirectory() as folder:
                target = Path(folder) / 'tool.tar.xz'
                calls = []
                def opener(request, timeout):
                    calls.append(request.full_url)
                    if request.full_url == url:
                        raise urllib.error.HTTPError(url, 504, 'gateway', {}, None)
                    if request.full_url == api + 'tags/test':
                        return io.BytesIO(json.dumps(metadata).encode())
                    self.assertEqual(request.full_url, asset)
                    return io.BytesIO(b'x' * len(payload) if corrupt else payload)
                if corrupt:
                    with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
                        download(target, dist, digest, opener)
                    self.assertFalse(target.exists())
                    self.assertFalse(target.with_suffix('.xz.part').exists())
                else:
                    download(target, dist, digest, opener)
                    self.assertEqual(target.read_bytes(), payload)
                    self.assertEqual(len(calls), 3)
                    download(target, dist, digest, opener)
                    self.assertEqual(len(calls), 3, 'verified archive is reused without network')


if __name__ == '__main__':
    unittest.main()
