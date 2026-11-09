"""Exercise the actual source downloader with isolated, deterministic network responses."""
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HELPER = ROOT / 'scripts/download_verified_source.sh'


class VerifiedSourceDownloadTest(unittest.TestCase):
    def run_download(self, mode, existing=None):
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
    if [[ "$last" == https://xorg.freedesktop.org/* ]]; then printf broken > "$output"; return 7; fi
    if [ "$MODE" = bad ]; then printf invalid > "$output"; else printf verified > "$output"; fi
}
download_verified_source https://xorg.freedesktop.org/archive/individual/xcb/source.tar.xz "$CHECKSUM" "$TARGET"
'''
            env = dict(os.environ, HELPER=str(HELPER), TARGET=str(target), MODE=mode,
                       CHECKSUM=hashlib.sha256(b'verified').hexdigest(), CALLS=str(root / 'calls'))
            proc = subprocess.run(['bash', '-c', shell], env=env, capture_output=True, text=True)
            calls = (root / 'calls').read_text().splitlines() if (root / 'calls').exists() else []
            return proc.returncode, target.read_bytes() if target.exists() else None, calls, list(root.glob('*.part.*'))

    def test_cache_hit_needs_no_network(self):
        code, data, calls, parts = self.run_download('fail', b'verified')
        self.assertEqual((code, data, calls, parts), (0, b'verified', [], []))

    def test_retry_uses_mirror_and_replaces_bad_cache_only_after_verification(self):
        code, data, calls, parts = self.run_download('good', b'bad cache')
        self.assertEqual((code, data, parts), (0, b'verified', []))
        self.assertEqual(len(calls), 2)
        self.assertTrue(calls[1].startswith('https://www.x.org/'))

    def test_network_or_checksum_failure_never_publish_partial(self):
        for mode in ('fail', 'bad'):
            code, data, calls, parts = self.run_download(mode, b'old invalid cache')
            self.assertNotEqual(code, 0)
            self.assertEqual(data, b'old invalid cache')
            self.assertEqual(parts, [])

if __name__ == '__main__':
    unittest.main()
