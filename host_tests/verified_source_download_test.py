"""Exercise the actual source downloader with isolated, deterministic network responses."""
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import shutil
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HELPER = ROOT / 'scripts/download_verified_source.sh'
sys.path.insert(0, str(ROOT / 'scripts'))
from prefetch_x11_sources import sources


class VerifiedSourceDownloadTest(unittest.TestCase):
    def run_download(self, mode, existing=None, url="https://xorg.freedesktop.org/archive/individual/xcb/source.tar.xz"):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            target = root / 'source.tar.xz'
            if existing is not None:
                target.write_bytes(existing)
            shell = r'''set -euo pipefail
source "$HELPER"
log() { :; }
err() { echo "$*" >&2; return 1; }
curl() {
    local output="" arg last=""
    while [ "$#" -gt 0 ]; do
        arg="$1"; shift
        if [ "$arg" = -o ]; then output="$1"; shift; else last="$arg"; fi
    done
    echo "$last" >> "$CALLS"
    if [ "$MODE" = fail ]; then printf broken > "$output"; return 7; fi
    if [[ "$last" == https://xorg.freedesktop.org/* || "$last" == https://github.com/dottedmag/* ]]; then printf broken > "$output"; return 7; fi
    if [ "$MODE" = bad ]; then printf invalid > "$output"; else printf verified > "$output"; fi
}
download_verified_source "$URL" "$CHECKSUM" "$TARGET"
'''
            env = dict(os.environ, HELPER=str(HELPER), TARGET=str(target), MODE=mode,
                       CHECKSUM=hashlib.sha256(b'verified').hexdigest(), CALLS=str(root / 'calls'), URL=url)
            proc = subprocess.run(['bash', '-c', shell], env=env, capture_output=True, text=True)
            calls = (root / 'calls').read_text().splitlines() if (root / 'calls').exists() else []
            return proc.returncode, target.read_bytes() if target.exists() else None, calls, list(root.glob('*.part.*'))

    def test_inventory_fails_closed_on_unrecognized_download(self):
        inventory = sources()
        self.assertTrue(any(url.endswith('/xcb-util-wm-0.4.2.tar.xz') for url in inventory))
        self.assertTrue(any('libsha1/archive/' in url and '$' not in url for url in inventory))
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'scripts').mkdir()
            for name in ('build_xcb_stack.sh', 'build_x11_client.sh', 'build_xwayland.sh'):
                shutil.copyfile(ROOT / 'scripts' / name, root / 'scripts' / name)
            path = root / 'scripts/build_xcb_stack.sh'
            path.write_text(path.read_text() + '\nfetch_and_unpack "https://example.invalid/new.zip" "$UNKNOWN" output\n')
            with self.assertRaisesRegex(ValueError, 'Unrecognized'):
                sources(root)

    def test_cache_hit_needs_no_network(self):
        code, data, calls, parts = self.run_download('fail', b'verified')
        self.assertEqual((code, data, calls, parts), (0, b'verified', [], []))

    def test_retry_uses_mirror_and_replaces_bad_cache_only_after_verification(self):
        code, data, calls, parts = self.run_download('good', b'bad cache')
        self.assertEqual((code, data, parts), (0, b'verified', []))
        self.assertEqual(len(calls), 2)
        self.assertTrue(calls[1].startswith('https://www.x.org/'))

    def test_github_source_can_use_codeload_mirror(self):
        code, data, calls, parts = self.run_download('good', url='https://github.com/dottedmag/libsha1/archive/abc.tar.gz')
        self.assertEqual((code, data, parts), (0, b'verified', []))
        self.assertEqual(calls[1], 'https://codeload.github.com/dottedmag/libsha1/tar.gz/abc')

    def test_network_or_checksum_failure_never_publish_partial(self):
        for mode in ('fail', 'bad'):
            code, data, calls, parts = self.run_download(mode, b'old invalid cache')
            self.assertNotEqual(code, 0)
            self.assertEqual(data, b'old invalid cache')
            self.assertEqual(parts, [])

if __name__ == '__main__':
    unittest.main()
